#include "core/capi/mpi_capi.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/adb_collector.hpp"
#include "adapters/ios/ios_adapter.hpp"
#include "core/discovery/discovery_service.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"
#include "core/session/session_store.hpp"
#include "core/util/time.hpp"

namespace {

using namespace mpi;

// One process-wide cancellation source. The UI has a single Cancel affordance
// and the core's operations are not nested, so a single flag is the honest
// model rather than a handle the caller has to thread through.
CancellationSource& cancel_source() {
  static CancellationSource src;
  return src;
}

char* dup_string(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out == nullptr) return nullptr;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

char* error_json(const std::string& message) {
  json::Value v = json::Value::object();
  v.set("error", json::Value::string(message));
  return dup_string(v.dump());
}

// Wraps an entry point so nothing can escape across the ABI. A std::bad_alloc
// or any other exception becomes a JSON error document.
template <typename Fn>
char* guard(Fn&& fn) {
  try {
    return dup_string(fn().dump(2));
  } catch (const std::exception& e) {
    return error_json(std::string("internal error: ") + e.what());
  } catch (...) {
    return error_json("internal error: unknown exception");
  }
}

std::string safe(const char* s) { return s == nullptr ? std::string() : std::string(s); }

discovery::DiscoveryService make_discovery(bool include_simulators) {
  discovery::DiscoveryService svc;
  svc.add_provider(std::make_shared<android::AdbAdapter>());
  auto ios_adapter = std::make_shared<ios::IosAdapter>();
  ios_adapter->set_include_simulators(include_simulators);
  svc.add_provider(std::move(ios_adapter));
  return svc;
}

discovery::ProviderOptions provider_options(int timeout_ms) {
  discovery::ProviderOptions po;
  po.cancel = cancel_source().token();
  po.command_timeout_ms = timeout_ms > 0 ? timeout_ms : 20000;
  return po;
}

const model::DeviceRef* find_device(const model::DiscoverySnapshot& snap,
                                    const std::string& id, bool& ambiguous) {
  const model::DeviceRef* hit = nullptr;
  ambiguous = false;
  for (const auto& d : snap.devices) {
    if (d.device_id != id) continue;
    if (hit != nullptr) {
      ambiguous = true;
      return nullptr;
    }
    hit = &d;
  }
  return hit;
}

bool session_id_is_safe(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  if (id == "." || id == "..") return false;
  for (const char c : id) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!allowed) return false;
  }
  return true;
}

std::vector<std::string> list_session_dirs(const std::string& root) {
  std::vector<std::string> out;
  DIR* d = ::opendir(root.c_str());
  if (!d) return out;
  while (struct dirent* e = ::readdir(d)) {
    const std::string name(e->d_name);
    if (name == "." || name == "..") continue;
    const std::string path = root + "/" + name;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    struct stat mst{};
    if (::stat((path + "/manifest.json").c_str(), &mst) != 0) continue;
    out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.rbegin(), out.rend());
  return out;
}

}  // namespace

extern "C" {

void mpi_string_free(char* s) { std::free(s); }

void mpi_cancel_all(void) { cancel_source().cancel(); }

void mpi_cancel_reset(void) {
  // A fresh source, because CancellationSource has no reset: a token already
  // handed out must stay cancelled so an in-flight operation still unwinds.
  static_cast<void>(0);
  cancel_source() = CancellationSource();
}

char* mpi_version_json(void) {
  return guard([] {
    json::Value v = json::Value::object();
    v.set("engine_version", json::Value::string(rules::engine_version()));
    v.set("ruleset_version", json::Value::string(rules::ruleset_version()));
    v.set("generated_at", json::Value::string(time_util::now_iso8601_utc()));
    return v;
  });
}

char* mpi_devices_json(int include_simulators, int timeout_ms) {
  return guard([&] {
    auto svc = make_discovery(include_simulators != 0);
    return svc.snapshot(provider_options(timeout_ms), /*include_apps=*/false)
        .to_json();
  });
}

char* mpi_apps_json(const char* device_id, int include_simulators,
                    int timeout_ms) {
  return guard([&] {
    const std::string device = safe(device_id);
    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
    if (device.empty()) {
      root.set("error", json::Value::string("device_id is required"));
      return root;
    }
    auto svc = make_discovery(include_simulators != 0);
    const auto snap = svc.snapshot(provider_options(timeout_ms),
                                   /*include_apps=*/true);
    bool ambiguous = false;
    const model::DeviceRef* dev = find_device(snap, device, ambiguous);
    if (ambiguous) {
      root.set("error",
               json::Value::string("device id '" + device +
                                   "' matches more than one platform"));
      return root;
    }
    if (dev == nullptr) {
      root.set("error",
               json::Value::string("no device with id '" + device + "'"));
      return root;
    }
    root.set("device", dev->to_json());
    root.set("listed_at", json::Value::string(snap.taken_at));
    json::Value apps = json::Value::array();
    for (const auto& a : snap.apps) {
      if (a.key.device_id == device) apps.push_back(a.to_json());
    }
    root.set("apps", std::move(apps));
    json::Value errs = json::Value::array();
    for (const auto& e : snap.provider_errors) errs.push_back(json::Value::string(e));
    root.set("provider_errors", std::move(errs));
    root.set("enumeration_failed", json::Value::boolean(snap.enumeration_failed));
    return root;
  });
}

char* mpi_preflight_json(const char* device_id, const char* app_identifier,
                         int include_simulators, int timeout_ms) {
  return guard([&] {
    const std::string device = safe(device_id);
    const std::string app = safe(app_identifier);
    auto svc = make_discovery(include_simulators != 0);
    const auto po = provider_options(timeout_ms);
    const auto caps = svc.probe(po);
    const auto snap = svc.snapshot(po, /*include_apps=*/!app.empty());

    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
    root.set("capabilities", caps.to_json());

    model::BuildProfile build;
    bool ambiguous = false;
    const model::DeviceRef* dev =
        device.empty() ? nullptr : find_device(snap, device, ambiguous);
    if (dev != nullptr) {
      root.set("device", dev->to_json());
      model::BuildFact form;
      form.key = "device.form";
      form.value = model::to_string(dev->form);
      form.source = model::FactSource::kDeviceProvider;
      form.observed_at = snap.taken_at;
      form.basis = "reported by " + dev->provider;
      build.upsert(std::move(form));
    } else {
      root.set("device", json::Value::null());
    }

    const model::AppEntry* target = nullptr;
    std::size_t matches = 0;
    for (const auto& a : snap.apps) {
      if (a.key.app_identifier != app) continue;
      if (dev != nullptr && a.key.device_id != dev->device_id) continue;
      ++matches;
      target = &a;
    }
    root.set("target", (matches == 1 && target != nullptr) ? target->to_json()
                                                           : json::Value::null());
    root.set("target_match_count",
             json::Value::integer(static_cast<std::int64_t>(matches)));
    root.set("build", build.to_json());
    root.set("diagnostic_eligibility",
             model::evaluate_eligibility(build,
                                         model::MeasurementMode::kDiagnostic)
                 .to_json());
    root.set("benchmark_eligibility",
             model::evaluate_eligibility(build,
                                         model::MeasurementMode::kBenchmark)
                 .to_json());
    return root;
  });
}

char* mpi_rules_json(void) {
  return guard([] {
    json::Value root = json::Value::object();
    root.set("ruleset_version", json::Value::string(rules::ruleset_version()));
    root.set("engine_version", json::Value::string(rules::engine_version()));
    json::Value arr = json::Value::array();
    for (const auto& r : rules::all_rules()) arr.push_back(r->describe());
    root.set("rules", std::move(arr));
    return root;
  });
}

char* mpi_sessions_json(const char* sessions_dir) {
  return guard([&] {
    const std::string root_dir = safe(sessions_dir);
    json::Value root = json::Value::object();
    json::Value arr = json::Value::array();
    for (const auto& id : list_session_dirs(root_dir)) {
      const auto loaded = session::load_package(root_dir + "/" + id);
      json::Value s = json::Value::object();
      s.set("session_id", json::Value::string(id));
      if (loaded.ok) {
        s.set("state",
              json::Value::string(session::to_string(loaded.manifest.state)));
        s.set("created_at", json::Value::string(loaded.manifest.created_at));
        s.set("finalized_at", json::Value::string(loaded.manifest.finalized_at));
        s.set("synthetic", json::Value::boolean(loaded.manifest.synthetic));
        json::Value cf = json::Value::array();
        for (const auto& f : loaded.checksum_failures) {
          cf.push_back(json::Value::string(f));
        }
        s.set("checksum_failures", std::move(cf));
      } else {
        // Listed with its error rather than hidden, so a half-written package
        // is visible instead of appearing not to exist.
        s.set("state", json::Value::string("unreadable"));
        s.set("error", json::Value::string(loaded.error));
        s.set("synthetic", json::Value::null());
        s.set("checksum_failures", json::Value::array());
      }
      arr.push_back(std::move(s));
    }
    root.set("sessions", std::move(arr));
    root.set("sessions_dir", json::Value::string(root_dir));
    return root;
  });
}

char* mpi_session_json(const char* sessions_dir, const char* session_id) {
  return guard([&] {
    const std::string id = safe(session_id);
    json::Value root = json::Value::object();
    if (!session_id_is_safe(id)) {
      root.set("error", json::Value::string("invalid session id"));
      return root;
    }
    const std::string dir = safe(sessions_dir) + "/" + id;
    const auto loaded = session::load_package(dir);
    if (!loaded.ok) {
      root.set("error", json::Value::string(loaded.error));
      return root;
    }
    json::ParseError perr;
    auto report = json::parse_file(dir + "/report.json", json::Limits{}, &perr);
    if (!report) {
      root.set("error",
               json::Value::string("session report is unreadable: " + perr.message));
      return root;
    }
    root = *report;
    json::Value checks = json::Value::array();
    for (const auto& f : loaded.checksum_failures) {
      checks.push_back(json::Value::string(f));
    }
    root.set("checksum_failures", std::move(checks));
    root.set("manifest_state",
             json::Value::string(session::to_string(loaded.manifest.state)));
    return root;
  });
}

char* mpi_record_json(const char* sessions_dir, const char* device_id,
                      const char* app_identifier, int duration_s, int sample_hz,
                      int collect_frames, int collect_cpu, int collect_memory,
                      int reset_frame_history, int timeout_ms) {
  return guard([&] {
    json::Value out = json::Value::object();
    const std::string device = safe(device_id);
    const std::string app = safe(app_identifier);
    if (device.empty() || app.empty()) {
      out.set("error", json::Value::string("device and app are both required"));
      return out;
    }

    auto svc = make_discovery(true);
    const auto po = provider_options(timeout_ms);
    const auto caps = svc.probe(po);
    const auto snap = svc.snapshot(po, /*include_apps=*/true);

    bool ambiguous = false;
    const model::DeviceRef* dev = find_device(snap, device, ambiguous);
    if (ambiguous) {
      out.set("error", json::Value::string("ambiguous device id"));
      return out;
    }
    if (dev == nullptr) {
      out.set("error",
              json::Value::string("no device with id '" + device + "'"));
      return out;
    }
    if (!dev->usable_for_capture()) {
      out.set("error",
              json::Value::string("device is " +
                                  std::string(model::to_string(dev->trust)) +
                                  " and cannot be used for capture"));
      return out;
    }

    const model::AppEntry* target = nullptr;
    std::size_t matches = 0;
    for (const auto& a : snap.apps) {
      if (a.key.app_identifier != app || a.key.device_id != device) continue;
      ++matches;
      target = &a;
    }
    if (matches == 0) {
      out.set("error",
              json::Value::string("'" + app + "' was not found on this device"));
      return out;
    }
    if (matches > 1) {
      out.set("error",
              json::Value::string("'" + app +
                                  "' matches more than one entry; narrow the "
                                  "target"));
      return out;
    }
    if (target->profiling == model::ProfilingAvailability::kUnavailable) {
      out.set("error", json::Value::string("'" + app + "' cannot be profiled: " +
                                           target->profiling_reason));
      return out;
    }

    if (dev->platform != model::Platform::kAndroid) {
      // The same refusal the CLI makes: no collector, so no session.
      out.set("error",
              json::Value::string(
                  "live capture is not implemented for " +
                  std::string(model::to_string(dev->platform)) +
                  " in this build: the xctrace collector is not wired to the "
                  "session controller yet. The target resolved successfully, "
                  "so the blocker is the collector, not this target."));
      out.set("target_resolved", json::Value::boolean(true));
      out.set("unsupported", json::Value::boolean(true));
      out.set("source_results", json::Value::array());
      return out;
    }

    const auto reval = svc.revalidate(*dev, target->key, target->processes, po);
    auto processes = reval.processes.empty() ? target->processes : reval.processes;
    if (processes.empty()) {
      out.set("error",
              json::Value::string("no live process of '" + app +
                                  "' could be resolved; start it on the device "
                                  "and retry"));
      return out;
    }

    session::SessionManifest manifest;
    manifest.session_id = session::new_session_id();
    manifest.created_at = time_util::now_iso8601_utc();
    manifest.tool_version = rules::engine_version();
    manifest.requested_mode = model::MeasurementMode::kDiagnostic;
    manifest.state = session::SessionState::kRecording;
    manifest.state_transitions.push_back(std::string("idle -> recording @ ") +
                                         manifest.created_at);

    model::NormalizedTrace trace;
    trace.session_id = manifest.session_id;
    trace.device = *dev;
    trace.capabilities = caps;
    trace.requested_mode = manifest.requested_mode;
    trace.target.app = target->key;
    trace.target.processes = processes;
    trace.target.runtime_state_at_capture = target->runtime_state;
    trace.target.discovery_scope = target->visibility_scope;

    session::CaptureConfig cfg;
    cfg.cancel = cancel_source().token();
    cfg.duration = std::chrono::milliseconds(
        static_cast<std::int64_t>(duration_s > 0 ? duration_s : 6) * 1000);
    cfg.sample_frequency_hz = sample_hz > 0 ? sample_hz : 200;
    cfg.frames = collect_frames != 0;
    cfg.cpu_samples = collect_cpu != 0;
    cfg.memory = collect_memory != 0;
    cfg.reset_frame_history = reset_frame_history != 0;

    android::AdbCollector collector;
    const auto capture = collector.capture(*dev, processes, cfg, trace);
    for (const auto& c : capture.source_results) trace.capabilities.upsert(c);

    json::Value srcs = json::Value::array();
    for (const auto& c : capture.source_results) srcs.push_back(c.to_json());
    out.set("source_results", std::move(srcs));
    out.set("elapsed_ms", json::Value::integer(capture.elapsed.count()));
    out.set("capture_config", cfg.to_json());

    if (!capture.started) {
      out.set("error", json::Value::string(capture.error));
      return out;
    }
    if (!capture.any_data) {
      out.set("error", json::Value::string(capture.error));
      out.set("session_written", json::Value::boolean(false));
      return out;
    }

    const auto norm = ingest::normalize(trace, cancel_source().token());
    for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = manifest.requested_mode;
    eopts.cancel = cancel_source().token();
    auto analysis = rules::analyze(trace, symbol_service, eopts);

    report::ReportOptions rep;
    const std::string md = report::to_markdown(trace, analysis, rep);
    const std::string js = report::to_json(trace, analysis, rep);

    manifest.state = trace.partial ? session::SessionState::kPartial
                                   : session::SessionState::kCompleted;
    manifest.finalized_at = time_util::now_iso8601_utc();
    manifest.state_transitions.push_back(std::string("recording -> ") +
                                         session::to_string(manifest.state) +
                                         " @ " + manifest.finalized_at);
    manifest.synthetic = trace.synthetic;
    manifest.partial_reasons = trace.partial_reasons;

    const auto written = session::write_package(safe(sessions_dir), manifest,
                                                trace, analysis, snap, md, js);
    if (!written.ok) {
      out.set("error", json::Value::string(written.error));
      return out;
    }
    out.set("session_written", json::Value::boolean(true));
    out.set("session_id", json::Value::string(manifest.session_id));
    out.set("frames",
            json::Value::integer(static_cast<std::int64_t>(trace.frames.size())));
    out.set("cpu_samples", json::Value::integer(
                               static_cast<std::int64_t>(trace.cpu_samples.size())));
    out.set("counters", json::Value::integer(
                            static_cast<std::int64_t>(trace.counters.size())));
    out.set("issues", json::Value::integer(
                          static_cast<std::int64_t>(analysis.issues.size())));
    return out;
  });
}

char* mpi_analyze_trace_json(const char* trace_path) {
  return guard([&] {
    json::Value out = json::Value::object();
    model::NormalizedTrace trace;
    ingest::ReadDiagnostics diag;
    ingest::ReadOptions opts;
    opts.cancel = cancel_source().token();
    const auto reader = ingest::read_any(safe(trace_path), opts, trace, diag);
    if (!reader.has_value()) {
      json::Value errs = json::Value::array();
      for (const auto& e : diag.errors) errs.push_back(json::Value::string(e));
      out.set("error", json::Value::string("no reader could read this file"));
      out.set("details", std::move(errs));
      return out;
    }
    if (trace.session_id.empty()) {
      trace.session_id = "imported-" + session::new_session_id();
    }
    for (const auto& w : diag.warnings) trace.ingestion_warnings.push_back(w);
    ingest::normalize(trace, cancel_source().token());

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = model::MeasurementMode::kDiagnostic;
    eopts.cancel = cancel_source().token();
    const auto analysis = rules::analyze(trace, symbol_service, eopts);
    report::ReportOptions rep;
    json::ParseError perr;
    auto parsed = json::parse(report::to_json(trace, analysis, rep),
                              json::Limits{}, &perr);
    if (!parsed) {
      out.set("error", json::Value::string("report serialisation failed"));
      return out;
    }
    out = *parsed;
    out.set("reader_id", json::Value::string(*reader));
    return out;
  });
}

}  /* extern "C" */
