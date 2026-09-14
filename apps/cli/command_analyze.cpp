#include <fstream>
#include <iostream>
#include <sstream>

#include "adapters/android/hprof_parser.hpp"
#include "apps/cli/cli.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/session/session_store.hpp"
#include "core/session/suppressions.hpp"

namespace mpi::cli {
namespace {

// "DET-02.long_task_ms=80" -> {"DET-02.long_task_ms", 80.0}
bool parse_threshold(const std::string& spec, std::pair<std::string, double>& out,
                     std::string& error) {
  const std::size_t eq = spec.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) {
    error = "expected RULE.threshold=value, got '" + spec + "'";
    return false;
  }
  const std::string key = spec.substr(0, eq);
  const std::string value = spec.substr(eq + 1);
  try {
    std::size_t consumed = 0;
    const double v = std::stod(value, &consumed);
    if (consumed != value.size()) {
      error = "'" + value + "' is not a number";
      return false;
    }
    out = {key, v};
    return true;
  } catch (...) {
    error = "'" + value + "' is not a number";
    return false;
  }
}

bool is_directory(const std::string& path) {
  std::ifstream f(path + "/manifest.json");
  return f.good();
}

// Loads the symbol artifacts the invocation names, binding each to the build
// identity it must match.
symbols::SymbolService build_symbol_service(const Invocation& inv,
                                            const model::NormalizedTrace& trace,
                                            std::vector<std::string>& notes) {
  symbols::SymbolService svc;

  // The expected identities come from the trace's build profile unless the
  // caller overrides them.
  std::string bundle_id = inv.flag("bundle-id");
  if (bundle_id.empty()) {
    if (const model::BuildFact* f = trace.build.find("js.bundle_hash")) {
      bundle_id = f->value;
    }
  }
  std::string native_build_id = inv.flag("native-build-id");
  if (native_build_id.empty()) {
    if (const model::BuildFact* f = trace.build.find("native.build_id")) {
      native_build_id = f->value;
    }
  }
  svc.set_expected_bundle_id(bundle_id);
  svc.set_expected_native_build_id(native_build_id);

  const std::string source_root = inv.flag("source-root");
  if (!source_root.empty()) svc.set_local_source_root(source_root);

  const std::string revision = inv.flag("source-revision");
  const bool dirty = inv.has_flag("source-dirty");
  if (!revision.empty() || dirty) svc.set_source_revision(revision, dirty);

  const std::string map_path = inv.flag("source-map");
  if (!map_path.empty()) {
    std::string err;
    if (auto map = symbols::load_source_map(map_path, json::Limits{}, &err)) {
      svc.add_source_map(std::move(*map));
    } else {
      // Spec C15 / G18: a bad map is refused, and the refusal is reported.
      notes.push_back("source map rejected: " + err);
    }
  }
  const std::string r8_path = inv.flag("r8-map");
  if (!r8_path.empty()) {
    std::string err;
    if (auto map = symbols::load_obfuscation_map(r8_path, &err)) {
      svc.add_obfuscation_map(std::move(*map));
    } else {
      notes.push_back("R8 mapping rejected: " + err);
    }
  }
  return svc;
}

}  // namespace

ExitCode cmd_analyze(const Invocation& inv) {
  if (inv.positional.empty()) {
    std::cerr << "error: analyze needs a session package directory or a trace "
                 "file\n";
    return ExitCode::kUsage;
  }
  const std::string input = inv.positional.front();

  // Thresholds first, so a typo fails before any work is done.
  rules::EngineOptions engine_opts;
  engine_opts.cancel = inv.global.cancel;
  for (const auto& f : inv.flags) {
    if (f.first != "threshold") continue;
    std::pair<std::string, double> parsed;
    std::string err;
    if (!parse_threshold(f.second, parsed, err)) {
      std::cerr << "error: --threshold " << err << "\n";
      return ExitCode::kUsage;
    }
    engine_opts.threshold_overrides.push_back(parsed);
  }
  // A project's suppression file, shared with the desktop app. Read before
  // the one-off --suppress flag so a flag can add to a list rather than
  // replacing it.
  const std::string suppressions_path = inv.flag("suppressions");
  if (!suppressions_path.empty()) {
    session::SuppressionFile file;
    const auto read = session::read_suppressions(suppressions_path, file);
    if (!read.ok) {
      std::cerr << "error: " << read.error << "\n";
      return ExitCode::kUsage;
    }
    for (const auto& r : read.rejected) {
      // Refused, and said out loud: a suppression that vanished quietly would
      // look like a finding that was never suppressed.
      warn("suppression refused -- " + r);
    }
    for (auto& s : file.to_engine()) {
      engine_opts.suppressions.push_back(std::move(s));
    }
  }

  const std::string suppress = inv.flag("suppress");
  if (!suppress.empty()) {
    const std::string reason = inv.flag("suppress-reason");
    if (reason.empty()) {
      // Spec H10: a suppression without a reason is not auditable.
      std::cerr << "error: --suppress requires --suppress-reason so the "
                   "suppression stays auditable\n";
      return ExitCode::kUsage;
    }
    rules::EngineOptions::Suppression s;
    s.rule_id = suppress;
    s.reason = reason;
    s.expiry = inv.flag("suppress-expiry");
    s.author = "cli";
    engine_opts.suppressions.push_back(std::move(s));
  }

  const std::string mode_s = inv.flag("mode", "diagnostic");
  if (mode_s != "diagnostic" && mode_s != "benchmark" && mode_s != "unknown_limited") {
    std::cerr << "error: --mode must be diagnostic, benchmark, or "
                 "unknown_limited\n";
    return ExitCode::kUsage;
  }
  engine_opts.mode = model::measurement_mode_from_string(mode_s);

  // Resolve the trace: either a session package or a bare trace file.
  std::string trace_path = input;
  std::string heap_path = inv.flag("heap-dump");
  std::vector<std::string> notes;
  if (is_directory(input)) {
    const auto loaded = session::load_package(input);
    if (!loaded.ok) {
      std::cerr << "error: " << loaded.error << "\n";
      return ExitCode::kCollectionError;
    }
    trace_path = loaded.trace_path;
    for (const auto& f : loaded.checksum_failures) {
      notes.push_back("checksum: " + f);
      warn("session package checksum problem: " + f);
    }
    // A dump stored in the package is used unless the operator named
    // another. Not using it would leave DET-06 reporting missing evidence
    // that the session is carrying.
    if (heap_path.empty()) heap_path = loaded.heap_path;
    if (loaded.manifest.state != session::SessionState::kCompleted) {
      notes.push_back("session state is '" +
                      std::string(session::to_string(loaded.manifest.state)) +
                      "', not 'completed'");
    }
  }

  ingest::ReadOptions read_opts;
  read_opts.cancel = inv.global.cancel;
  read_opts.mark_synthetic = inv.has_flag("synthetic");
  if (inv.has_flag("max-input-mib")) {
    const long long mib = std::atoll(inv.flag("max-input-mib").c_str());
    if (mib <= 0) {
      std::cerr << "error: --max-input-mib must be a positive integer\n";
      return ExitCode::kUsage;
    }
    read_opts.json_limits.max_bytes =
        static_cast<std::size_t>(mib) * 1024ull * 1024ull;
  }

  model::NormalizedTrace trace;
  ingest::ReadDiagnostics diag;
  const auto reader_id = ingest::read_any(trace_path, read_opts, trace, diag);
  if (!reader_id.has_value()) {
    for (const auto& e : diag.errors) std::cerr << "error: " << e << "\n";
    return diag.errors.empty() ? ExitCode::kUnsupportedOperation
                               : ExitCode::kCollectionError;
  }
  if (trace.session_id.empty()) {
    trace.session_id = "imported-" + session::new_session_id();
  }
  for (const auto& w : diag.warnings) trace.ingestion_warnings.push_back(w);
  notes.push_back("read with " + *reader_id + ": " +
                  std::to_string(diag.events_read) + " event(s) kept, " +
                  std::to_string(diag.events_rejected) + " rejected");
  if (diag.cancelled) {
    trace.partial = true;
    trace.partial_reasons.push_back("ingestion cancelled by the operator");
  }

  heap::HeapGraph heap_graph;
  if (!heap_path.empty()) {
    android::HprofLimits hlimits;
    const auto hr =
        android::read_hprof(heap_path, hlimits, inv.global.cancel, heap_graph);
    if (hr.ok && !heap_graph.objects().empty()) {
      // A dump taken by this tool's own `--heap` had a collection requested
      // first; one handed over by an operator may not have, and that changes
      // what presence in it means. The flag is set only for the path this
      // tool wrote.
      heap_graph.gc_requested_before_dump = inv.flag("heap-dump").empty();
      notes.push_back("heap dump read from " + heap_path + ": " +
                      std::to_string(heap_graph.objects().size()) +
                      " object(s), " +
                      std::to_string(heap_graph.roots().size()) + " root(s)");
    } else {
      notes.push_back(
          "the heap dump at " + heap_path + " could not be read (" +
          (hr.error.empty() ? std::string("no objects found") : hr.error) +
          "), so no reference paths are available");
      heap_graph = heap::HeapGraph{};
    }
  }

  const auto norm = ingest::normalize(trace, inv.global.cancel);
  for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);
  if (norm.duplicates_flagged > 0) {
    trace.ingestion_warnings.push_back(
        std::to_string(norm.duplicates_flagged) +
        " event(s) carried duplicate ids and are flagged");
  }
  if (norm.out_of_order_flagged > 0) {
    trace.ingestion_warnings.push_back(
        std::to_string(norm.out_of_order_flagged) +
        " event(s) arrived out of monotonic order and are flagged");
  }
  if (norm.incomplete_spans > 0) {
    trace.ingestion_warnings.push_back(
        std::to_string(norm.incomplete_spans) +
        " span(s) are incomplete; their durations remain unknown, not zero");
  }

  auto symbol_service = build_symbol_service(inv, trace, notes);
  for (auto& b : symbol_service.bindings()) {
    trace.build.symbol_bindings.push_back(b);
  }

  if (!heap_graph.objects().empty()) engine_opts.heap_graph = &heap_graph;
  auto analysis = rules::analyze(trace, symbol_service, engine_opts);
  for (const auto& n : notes) analysis.data_quality_notes.push_back(n);

  report::ReportOptions ropts;
  ropts.include_source_paths = inv.has_flag("include-source-paths");
  ropts.include_raw_events = inv.has_flag("include-events");

  const std::string format = inv.flag("format", inv.global.json ? "json" : "markdown");
  std::string rendered;
  if (format == "json") {
    rendered = report::to_json(trace, analysis, ropts);
  } else if (format == "markdown" || format == "md") {
    rendered = report::to_markdown(trace, analysis, ropts);
  } else {
    std::cerr << "error: --format must be json or markdown\n";
    return ExitCode::kUsage;
  }

  const std::string out_path = inv.flag("out");
  if (out_path.empty()) {
    std::cout << rendered;
  } else {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot write " << out_path << "\n";
      return ExitCode::kCollectionError;
    }
    f.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
    if (!f) {
      std::cerr << "error: write to " << out_path << " failed\n";
      return ExitCode::kCollectionError;
    }
    std::cerr << "wrote " << out_path << "\n";
  }

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;

  // The exit code reflects what the analysis could establish, not merely
  // whether the program ran.
  bool any_observed = false;
  bool any_ran = false;
  for (const auto& r : analysis.rule_runs) {
    if (r.outcome != model::RuleOutcome::kSkipped) any_ran = true;
  }
  for (const auto& i : analysis.issues) {
    if (i.suppressed) continue;
    if (i.detection_status == model::DetectionStatus::kObserved) any_observed = true;
  }
  if (!any_ran) {
    // Every detector was skipped: that is not a clean result.
    return ExitCode::kInconclusive;
  }
  if (trace.partial && !any_observed) return ExitCode::kInconclusive;
  return ExitCode::kOk;
}

}  // namespace mpi::cli
