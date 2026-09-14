#include "core/rules/engine.hpp"

#include <algorithm>

#include "core/rules/rule_registry.hpp"
#include "core/util/time.hpp"

namespace mpi::rules {

std::string engine_version() { return "0.1.0"; }

model::AnalysisResult analyze(const model::NormalizedTrace& trace,
                              const symbols::SymbolService& symbol_service,
                              const EngineOptions& opts) {
  model::AnalysisResult result;
  result.session_id = trace.session_id;
  result.engine_version = engine_version();
  result.ruleset_version = ruleset_version();
  result.analyzed_at = time_util::now_iso8601_utc();
  result.mode = opts.mode;
  result.contains_synthetic_data = trace.synthetic;
  result.coverage = trace.coverage;

  // Eligibility is computed once, from the build profile, and stamped onto
  // every issue. A diagnostic session can never come out as a benchmark pass.
  result.eligibility = model::evaluate_eligibility(trace.build, opts.mode);

  if (trace.synthetic) {
    result.data_quality_notes.push_back(
        "SYNTHETIC INPUT: this analysis ran on a labelled fixture, not a real "
        "device capture. No conclusion here describes real app behavior." +
        (trace.synthetic_note.empty() ? std::string()
                                      : " Note: " + trace.synthetic_note));
  }
  if (trace.partial) {
    result.data_quality_notes.push_back(
        "PARTIAL CAPTURE: the recording ended abnormally, so absence of a "
        "finding is not evidence of absence.");
    for (const auto& r : trace.partial_reasons) {
      result.data_quality_notes.push_back("partial capture reason: " + r);
    }
  }
  for (const auto& w : trace.ingestion_warnings) {
    result.data_quality_notes.push_back("ingestion: " + w);
  }
  for (const auto& kv : trace.dropped_events_by_collector) {
    if (kv.second <= 0) continue;
    result.data_quality_notes.push_back(
        kv.first + " dropped " + std::to_string(kv.second) +
        " event(s); the affected intervals are gaps, not measured zero "
        "activity");
  }
  // Ambiguous process ownership is stated rather than folded into totals.
  for (const auto& p : trace.target.processes) {
    if (p.counts_toward_app_totals()) continue;
    result.data_quality_notes.push_back(
        "process " + std::to_string(p.pid) + " (" + p.process_name +
        ") has " + model::to_string(p.ownership) +
        " ownership evidence and is excluded from app-scoped totals" +
        (p.ownership_note.empty() ? std::string() : ": " + p.ownership_note));
  }
  if (trace.target.discovery_scope == model::DiscoveryScope::kPartial) {
    result.data_quality_notes.push_back(
        "the app list this target came from was a partial listing, so other "
        "processes of this app may exist unobserved");
  }

  RuleContext ctx;
  ctx.trace = &trace;
  ctx.symbol_service = &symbol_service;
  ctx.mode = opts.mode;
  ctx.eligibility = result.eligibility;
  ctx.cancel = opts.cancel;
  ctx.threshold_overrides = opts.threshold_overrides;

  for (const auto& rule : all_rules()) {
    if (opts.cancel.cancelled()) {
      model::RuleRunRecord rec;
      rec.rule_id = rule->id();
      rec.rule_version = rule->version();
      rec.outcome = model::RuleOutcome::kSkipped;
      rec.skipped_reasons.push_back("analysis cancelled before this rule ran");
      result.rule_runs.push_back(std::move(rec));
      continue;
    }
    RuleOutput out = rule->run(ctx);
    for (auto& issue : out.issues) {
      // Suppressions are applied here, never by dropping the issue: the
      // reason and expiry travel into the export (spec section 13, H10).
      for (const auto& s : opts.suppressions) {
        if (s.rule_id != issue.rule_id) continue;
        if (!s.fingerprint.empty() && s.fingerprint != issue.fingerprint) continue;
        issue.suppressed = true;
        issue.suppression_reason = s.reason;
        issue.suppression_expiry = s.expiry;
        issue.suppression_author = s.author;
        break;
      }
      result.issues.push_back(std::move(issue));
    }
    for (auto& a : out.attribution) result.attribution.push_back(std::move(a));
    result.rule_runs.push_back(std::move(out.record));
  }

  // Impact order for display; ties broken by fingerprint so the order is
  // stable across re-analysis of the same input.
  std::stable_sort(result.issues.begin(), result.issues.end(),
                   [](const model::Issue& a, const model::Issue& b) {
                     if (a.severity != b.severity) {
                       return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                     }
                     return a.fingerprint < b.fingerprint;
                   });
  return result;
}

}  // namespace mpi::rules
