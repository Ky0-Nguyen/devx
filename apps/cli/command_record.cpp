// `mpi record`
//
// M1 has no on-device collector. That is a real gap, and this command states
// it rather than producing a session that looks like a capture.
//
// Spec section 0.5 forbids reporting a dashboard backed by synthetic data as a
// working profiler; section 0.16 forbids silently substituting import-only
// support for live capture; and the closing instruction requires an
// unavailable live capability to be documented as a blocker instead of being
// disguised as fulfilment.
//
// So `record` does everything it legitimately can -- revalidate the pinned
// target, resolve capabilities, and build a real session package -- and then
// either imports a trace the caller supplies (labelled as an import, not a
// capture) or exits `unsupported` with the blocker spelled out.
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include "adapters/android/adb_collector.hpp"
#include "adapters/ios/xctrace_collector.hpp"

#include "apps/cli/cli.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/session/live_capture.hpp"
#include "core/session/session_store.hpp"
#include "core/util/time.hpp"

namespace mpi::cli {

ExitCode cmd_record(const Invocation& inv) {
  if (inv.global.app.empty()) {
    std::cerr << "error: record needs --app <package-name|bundle-id>\n\n"
                 "Targets are selected by identifier. A PID is never "
                 "required.\n";
    return ExitCode::kUsage;
  }

  // Argument validation first. A misspelled launch class is a usage error and
  // reporting it should not depend on a device being reachable.
  const std::string requested_launch_class = inv.flag("launch-class", "cold");
  if (requested_launch_class != "cold" && requested_launch_class != "warm" &&
      requested_launch_class != "hot") {
    std::cerr << "error: --launch-class must be cold, warm or hot\n";
    return ExitCode::kUsage;
  }
  if (inv.has_flag("wait-for-app-s") &&
      std::atoi(inv.flag("wait-for-app-s").c_str()) <= 0) {
    std::cerr << "error: --wait-for-app-s must be a positive integer\n";
    return ExitCode::kUsage;
  }

  auto svc = make_discovery(inv.global);
  const auto po = provider_options(inv.global);

  session::SessionManifest manifest;
  manifest.session_id = session::new_session_id();
  manifest.created_at = time_util::now_iso8601_utc();
  manifest.tool_version = rules::engine_version();
  manifest.requested_mode =
      model::measurement_mode_from_string(inv.flag("mode", "diagnostic"));
  manifest.state = session::SessionState::kDiscovering;
  manifest.state_transitions.push_back(std::string("idle -> discovering @ ") +
                                       manifest.created_at);

  const auto caps = svc.probe(po);
  const auto snap = svc.snapshot(po, /*include_apps=*/true);

  model::DeviceRef device;
  ExitCode code = ExitCode::kOk;
  if (!resolve_device(inv, snap, device, code)) return code;

  manifest.state = session::SessionState::kPreflight;
  manifest.state_transitions.push_back(
      std::string("discovering -> preflight @ ") + time_util::now_iso8601_utc());

  // Pin the selected identifier. The tool follows only this identifier; it
  // never retargets to a different app (spec section 6, 3.2).
  const model::AppEntry* target = nullptr;
  std::vector<const model::AppEntry*> matches;
  for (const auto& a : snap.apps) {
    if (a.key.app_identifier != inv.global.app) continue;
    if (a.key.device_id != device.device_id) continue;
    matches.push_back(&a);
  }
  if (matches.empty()) {
    std::cerr << "error: '" << inv.global.app << "' was not found on "
              << device.device_id << ".\n"
              << "       Run `mpi apps --device " << device.device_id
              << "` to see what is listed, and note whether the listing's "
                 "scope is partial.\n";
    return ExitCode::kNotFound;
  }
  if (matches.size() > 1) {
    std::cerr << "error: '" << inv.global.app << "' matches " << matches.size()
              << " entries on this device (different Android users). Narrow "
                 "the target.\n";
    for (const auto* m : matches) std::cerr << "  - " << m->key.canonical() << "\n";
    return ExitCode::kAmbiguousTarget;
  }
  target = matches.front();

  // Revalidate immediately before recording: the app may have exited or
  // restarted since the listing (spec A17).
  const auto reval = svc.revalidate(device, target->key, target->processes, po);
  for (const auto& n : reval.notes) std::cerr << "note: " << n << "\n";
  for (const auto& e : reval.errors) std::cerr << "warning: " << e << "\n";

  if (target->profiling == model::ProfilingAvailability::kUnavailable) {
    std::cerr << "error: '" << inv.global.app
              << "' cannot be profiled: " << target->profiling_reason << "\n";
    if (!target->profiling_recovery_action.empty()) {
      std::cerr << "       fix: " << target->profiling_recovery_action << "\n";
    }
    return ExitCode::kUnsupportedOperation;
  }
  const bool launch_requested = inv.has_flag("launch");
  if (!reval.app_still_present &&
      target->runtime_state != model::RuntimeState::kRunning &&
      !launch_requested) {
    std::cerr << "error: no live process of '" << inv.global.app
              << "' could be resolved.\n"
                 "       Start it on the device, or record with --launch to "
                 "have the app started and its startup measured.\n";
    return ExitCode::kNotFound;
  }

  model::NormalizedTrace trace;
  trace.session_id = manifest.session_id;
  trace.device = device;
  trace.capabilities = caps;
  trace.requested_mode = manifest.requested_mode;
  trace.target.app = target->key;
  trace.target.processes =
      reval.processes.empty() ? target->processes : reval.processes;
  trace.target.runtime_state_at_capture = target->runtime_state;
  trace.target.discovery_scope = target->visibility_scope;

  const std::string import_path = inv.flag("import");

  // A collector exists for Android. If one exists for the selected platform,
  // capture for real; otherwise say so rather than producing a capture-shaped
  // file with nothing measured.
  std::unique_ptr<session::Collector> collector;
  if (import_path.empty() && device.platform == model::Platform::kAndroid) {
    collector = std::make_unique<android::AdbCollector>();
  } else if (import_path.empty() && device.platform == model::Platform::kIos) {
    collector = std::make_unique<ios::XctraceCollector>();
  }

  if (collector) {
    session::CaptureConfig cfg;
    cfg.cancel = inv.global.cancel;
    cfg.preset = inv.flag("preset", "lightweight");
    if (inv.has_flag("duration-s")) {
      const int secs = std::atoi(inv.flag("duration-s").c_str());
      if (secs <= 0) {
        std::cerr << "error: --duration-s must be a positive integer\n";
        return ExitCode::kUsage;
      }
      cfg.duration = std::chrono::milliseconds(secs * 1000);
    }
    if (inv.has_flag("sample-hz")) {
      const int hz = std::atoi(inv.flag("sample-hz").c_str());
      if (hz <= 0) {
        std::cerr << "error: --sample-hz must be a positive integer\n";
        return ExitCode::kUsage;
      }
      cfg.sample_frequency_hz = hz;
    }
    cfg.launch_app = launch_requested;
    cfg.launch_class = requested_launch_class;
    if (inv.has_flag("wait-for-app-s")) {
      cfg.wait_for_app = std::chrono::milliseconds(
          std::atoi(inv.flag("wait-for-app-s").c_str()) * 1000);
    } else if (launch_requested) {
      // A launched app needs a moment to appear in the process list before
      // the collector can pin it; without this the capture would begin
      // against a target that does not exist yet.
      cfg.wait_for_app = std::chrono::milliseconds(10000);
    }
    if (inv.has_flag("no-frames")) cfg.frames = false;
    if (inv.has_flag("no-cpu")) cfg.cpu_samples = false;
    if (inv.has_flag("no-memory")) cfg.memory = false;
    if (inv.has_flag("no-frame-reset")) cfg.reset_frame_history = false;

    manifest.state = session::SessionState::kRecording;
    manifest.state_transitions.push_back(
        std::string("preflight -> recording @ ") + time_util::now_iso8601_utc());

    if (cfg.launch_app) {
      std::cerr << "launching " << inv.global.app << " (" << cfg.launch_class
                << ") ...\n";
      const auto launched =
          collector->launch(device, inv.global.app, cfg, trace);
      for (const auto& c : launched.source_results) trace.capabilities.upsert(c);
      for (const auto& n : launched.notes) std::cerr << "note: " << n << "\n";
      if (!launched.supported) {
        std::cerr << "error: " << launched.error << "\n";
        return ExitCode::kUnsupportedOperation;
      }
      if (!launched.started) {
        // Not fatal: the app may well be running, and the rest of the capture
        // is still worth having. What is refused is a startup *measurement*.
        warn(launched.error);
      } else {
        std::cerr << "launched " << inv.global.app;
        if (!launched.launch_class.empty()) {
          std::cerr << " (" << launched.launch_class << " per the platform)";
        }
        if (launched.total_time_ns.has_value()) {
          std::cerr << ", TotalTime "
                    << (*launched.total_time_ns / 1000000) << " ms";
        }
        if (launched.displayed_ns.has_value()) {
          std::cerr << ", Displayed " << (*launched.displayed_ns / 1000000)
                    << " ms";
        }
        std::cerr << "\n";
      }

      // Wait for the process the launch created, and re-resolve identity
      // against it: the pid that existed before the launch is not the one to
      // capture (spec A21, A22).
      if (cfg.wait_for_app.count() > 0) {
        const auto waited = svc.wait_for_app_process(
            device, target->key, cfg.wait_for_app,
            std::chrono::milliseconds(250), po);
        for (const auto& n : waited.notes) std::cerr << "note: " << n << "\n";
        if (waited.cancelled) return ExitCode::kCancelled;
        if (!waited.appeared) {
          std::cerr << "error: '" << inv.global.app
                    << "' did not appear as a running process within "
                    << (cfg.wait_for_app.count() / 1000)
                    << "s of the launch.\n";
          return ExitCode::kNotFound;
        }
        trace.target.processes = waited.processes;
        trace.target.runtime_state_at_capture = model::RuntimeState::kRunning;
      }
    }

    if (inv.has_flag("tick-ms")) {
      const int ms = std::atoi(inv.flag("tick-ms").c_str());
      if (ms < 100) {
        std::cerr << "error: --tick-ms must be at least 100\n";
        return ExitCode::kUsage;
      }
      cfg.tick_interval = std::chrono::milliseconds(ms);
    }

    // --live streams: the collector ticks and the numbers are printed as they
    // arrive, rather than the command blocking and reporting at the end.
    const bool live = inv.has_flag("live");
    session::CaptureResult capture;
    if (live && collector->supports_streaming()) {
      session::LiveSession session;
      if (!session.start(std::move(collector), device, trace.target.processes,
                         cfg, std::move(trace))) {
        const auto live_snap = session.snapshot();
        std::cerr << "error: live capture could not start: " << live_snap.error
                  << "\n";
        return ExitCode::kCollectionError;
      }
      if (!inv.global.quiet) {
        std::cerr << "streaming " << inv.global.app << " on "
                  << device.device_id << " every " << cfg.tick_interval.count()
                  << " ms -- Ctrl-C to stop"
                  << (cfg.duration.count() > 0
                          ? (" (auto-stop after " +
                             std::to_string(cfg.duration.count() / 1000) + "s)")
                          : std::string())
                  << "\n";
      }

      const auto deadline = std::chrono::steady_clock::now() + cfg.duration;
      std::int64_t printed = 0;
      while (session.running() && !inv.global.cancel.cancelled()) {
        if (cfg.duration.count() > 0 &&
            std::chrono::steady_clock::now() >= deadline) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto live_snap = session.snapshot();
        if (live_snap.ticks == printed) continue;
        printed = live_snap.ticks;
        if (inv.global.quiet) continue;
        // One line per tick. Live numbers are labelled every time, so a line
        // copied out of a terminal cannot be mistaken for a final result.
        std::cerr << "  [" << std::setw(6) << live_snap.elapsed.count()
                  << " ms] frames=" << live_snap.frames
                  << " samples=" << live_snap.cpu_samples
                  << " counters=" << live_snap.counter_points
                  << " issues(preliminary)="
                  << live_snap.preliminary_analysis.issues.size()
                  << " tick=" << live_snap.last_tick_cost.count() << "ms";
        for (const auto& kv : live_snap.latest_counters) {
          if (kv.first != "memory.rss_total_bytes") continue;
          std::cerr << " rss=" << static_cast<long long>(kv.second / 1048576)
                    << "MB";
        }
        std::cerr << "\n";
      }

      session.stop();
      capture = session.result();
      trace = session.take_trace();
      for (const auto& c : capture.source_results) trace.capabilities.upsert(c);
      if (!inv.global.quiet) {
        const auto live_snap = session.snapshot();
        std::cerr << "stopped after " << live_snap.ticks
                  << " tick(s); collector spent "
                  << live_snap.collector_cost.count()
                  << " ms interacting with the device\n";
      }
    } else {
      if (live) {
        warn("this collector does not support streaming; falling back to a "
             "batch capture of " +
             std::to_string(cfg.duration.count() / 1000) + "s");
      }
      if (!inv.global.quiet) {
        std::cerr << "recording " << inv.global.app << " on " << device.device_id
                  << " for " << (cfg.duration.count() / 1000) << "s via "
                  << collector->id() << " ...\n";
      }
      capture = collector->capture(device, trace.target.processes, cfg, trace);
    }

    manifest.state = session::SessionState::kProcessing;
    manifest.state_transitions.push_back(
        std::string("recording -> processing @ ") + time_util::now_iso8601_utc());

    // Per-source results go into the capability matrix, so the report can show
    // exactly which collectors ran and which could not.
    for (const auto& c : capture.source_results) trace.capabilities.upsert(c);

    if (!inv.global.quiet) {
      std::cerr << "capture finished in " << capture.elapsed.count() << " ms\n";
      for (const auto& c : capture.source_results) {
        std::cerr << "  " << c.id << ": " << model::to_string(c.status);
        if (!c.evidence.empty()) std::cerr << " -- " << c.evidence;
        std::cerr << "\n";
        for (const auto& l : c.limitations) {
          std::cerr << "      limitation: " << l << "\n";
        }
        if (!c.recovery_action.empty()) {
          std::cerr << "      fix: " << c.recovery_action << "\n";
        }
      }
    }

    if (!capture.started) {
      std::cerr << "error: capture could not start: " << capture.error << "\n";
      return ExitCode::kCollectionError;
    }
    if (!capture.any_data) {
      // A capture that measured nothing is not written as a session: that is
      // the capture-shaped-but-empty artefact spec section 0.5 forbids.
      std::cerr << "error: " << capture.error << "\n"
                << "       No session was written. Every source's status is "
                   "above; a `permission_denied` CPU source usually means the "
                   "target is not debuggable or profileable.\n";
      return ExitCode::kCollectionError;
    }

    const auto norm = ingest::normalize(trace, inv.global.cancel);
    for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = manifest.requested_mode;
    eopts.cancel = inv.global.cancel;
    auto analysis = rules::analyze(trace, symbol_service, eopts);

    report::ReportOptions rep_opts;
    const std::string md = report::to_markdown(trace, analysis, rep_opts);
    const std::string js = report::to_json(trace, analysis, rep_opts);

    manifest.state = trace.partial ? session::SessionState::kPartial
                                   : session::SessionState::kCompleted;
    manifest.finalized_at = time_util::now_iso8601_utc();
    manifest.state_transitions.push_back(
        std::string("processing -> ") + session::to_string(manifest.state) +
        " @ " + manifest.finalized_at);
    manifest.synthetic = trace.synthetic;
    manifest.partial_reasons = trace.partial_reasons;

    const auto written = session::write_package(
        inv.global.sessions_dir, manifest, trace, analysis, snap, md, js);
    if (!written.ok) {
      std::cerr << "error: " << written.error << "\n";
      return ExitCode::kCollectionError;
    }
    std::cerr << "session written: " << written.package_dir << "\n";
    std::cout << "session_id=" << manifest.session_id << "\n";
    std::cout << "package=" << written.package_dir << "\n";
    std::cout << "kind=live capture (" << collector->id() << ")\n";
    std::cout << "frames=" << trace.frames.size()
              << " cpu_samples=" << trace.cpu_samples.size()
              << " counters=" << trace.counters.size() << "\n";
    std::cout << "issues=" << analysis.issues.size() << "\n";
    if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
    return ExitCode::kOk;
  }

  if (import_path.empty()) {
    // No collector for this platform yet. (Reached only when `collector` was
    // never created; the streaming path above moves it and returns.)
    std::cerr
        << "\nUNSUPPORTED: live capture is not implemented for "
        << model::to_string(device.platform) << " in this build.\n\n"
           "Implemented today:\n"
           "  - Android live capture (frames via dumpsys gfxinfo framestats, "
           "CPU samples via simpleperf, memory via dumpsys meminfo)\n"
           "  - device discovery and app enumeration on Android and iOS\n"
           "  - capability preflight, analysis, issues, reports, comparison\n\n"
           "Not implemented:\n"
           "  - iOS live capture: the xctrace collector is not wired to the "
           "session controller yet\n\n"
           "Supported path for this target today:\n"
           "  mpi record --device "
        << device.device_id << " --app " << inv.global.app
        << " --import <trace-file>\n"
           "      builds a real session package from an existing trace, "
           "labelled an import rather than a capture.\n\n"
           "The target resolved successfully, so the blocker is the collector, "
           "not this target:\n";
    std::cerr << "  app:     " << target->key.canonical() << "\n";
    std::cerr << "  state:   " << model::to_string(target->runtime_state) << "\n";
    std::cerr << "  procs:   " << trace.target.processes.size() << "\n";
    for (const auto& p : trace.target.processes) {
      std::cerr << "    pid " << p.pid << " (" << model::to_string(p.ownership)
                << (p.counts_toward_app_totals() ? ", counted" : ", EXCLUDED")
                << ")\n";
    }
    return ExitCode::kUnsupportedOperation;
  }

  // Import path: a real session package, honestly labelled.
  manifest.state = session::SessionState::kProcessing;
  manifest.state_transitions.push_back(
      std::string("preflight -> processing (import, no live capture) @ ") +
      time_util::now_iso8601_utc());

  ingest::ReadOptions ropts;
  ropts.cancel = inv.global.cancel;
  ropts.mark_synthetic = inv.has_flag("synthetic");
  ingest::ReadDiagnostics diag;
  const auto reader_id = ingest::read_any(import_path, ropts, trace, diag);
  if (!reader_id.has_value()) {
    for (const auto& e : diag.errors) std::cerr << "error: " << e << "\n";
    return ExitCode::kCollectionError;
  }
  // The reader may have overwritten identity fields from the file; the target
  // the operator selected wins, and the difference is recorded.
  trace.session_id = manifest.session_id;
  trace.device = device;
  trace.capabilities = caps;
  trace.target.app = target->key;
  trace.ingestion_warnings.push_back(
      "this session was built by importing " + import_path + " via " +
      *reader_id + "; it is NOT a live capture of the selected app, and its "
      "events were not collected by this tool");
  for (const auto& w : diag.warnings) trace.ingestion_warnings.push_back(w);

  const auto norm = ingest::normalize(trace, inv.global.cancel);
  for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);

  symbols::SymbolService symbol_service;
  rules::EngineOptions eopts;
  eopts.mode = manifest.requested_mode;
  eopts.cancel = inv.global.cancel;
  auto analysis = rules::analyze(trace, symbol_service, eopts);
  analysis.data_quality_notes.push_back(
      "IMPORTED SESSION: the trace was supplied by the operator rather than "
      "captured by this tool, so the link between these events and the "
      "selected app identifier is asserted, not observed");

  report::ReportOptions rep_opts;
  const std::string md = report::to_markdown(trace, analysis, rep_opts);
  const std::string js = report::to_json(trace, analysis, rep_opts);

  manifest.state = session::SessionState::kCompleted;
  manifest.finalized_at = time_util::now_iso8601_utc();
  manifest.state_transitions.push_back(
      std::string("processing -> completed @ ") + manifest.finalized_at);
  manifest.synthetic = trace.synthetic;
  manifest.partial_reasons = trace.partial_reasons;
  if (trace.partial) manifest.state = session::SessionState::kPartial;

  const auto written = session::write_package(inv.global.sessions_dir, manifest,
                                              trace, analysis, snap, md, js);
  if (!written.ok) {
    std::cerr << "error: " << written.error << "\n";
    return ExitCode::kCollectionError;
  }
  std::cerr << "session written: " << written.package_dir << "\n";
  std::cout << "session_id=" << manifest.session_id << "\n";
  std::cout << "package=" << written.package_dir << "\n";
  std::cout << "kind=import (not a live capture)\n";

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  return ExitCode::kOk;
}

}  // namespace mpi::cli
