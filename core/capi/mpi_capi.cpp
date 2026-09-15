#include "core/capi/mpi_capi.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/adb_collector.hpp"
#include "adapters/android/hprof_parser.hpp"
#include "adapters/ios/ios_adapter.hpp"
#include "adapters/ios/xctrace_collector.hpp"
#include "core/discovery/boot.hpp"
#include "core/discovery/discovery_service.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"
#include "core/session/live_capture.hpp"
#include "core/session/compare.hpp"
#include "core/session/session_store.hpp"
#include "core/session/suppressions.hpp"
#include "core/timeline/timeline.hpp"
#include "core/util/time.hpp"

namespace {

using namespace mpi;

// One process-wide cancellation source, behind a lock.
//
// The UI has a single Cancel affordance, so a single flag is the honest model.
// It must be guarded, though: the source was previously a bare static that
// mpi_live_start reassigned on every start, while concurrent calls -- a device
// listing and a session listing both run when DevX opens -- were copying
// tokens out of it. Reassigning a shared_ptr while other threads read it is a
// data race, and it hung the UI on "Starting live capture..." rather than
// failing visibly.
class CancelRegistry {
 public:
  CancellationToken token() {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_.token();
  }
  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_.cancel();
  }
  // Replaces the source so a new operation starts uncancelled. Tokens already
  // handed out keep referring to the old flag, which is what callers of a
  // cancelled operation should see.
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = CancellationSource();
  }

 private:
  std::mutex mutex_;
  CancellationSource source_;
};

CancelRegistry& cancel_registry() {
  static CancelRegistry reg;
  return reg;
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
  po.cancel = cancel_registry().token();
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

// The single live session this process can run, plus the context needed to
// finalise it. Guarded because the UI polls from its own thread.
struct LiveContext {
  std::mutex mutex;
  std::unique_ptr<session::LiveSession> live;
  session::SessionManifest manifest;
  model::DiscoverySnapshot discovery;
  std::string sessions_dir;
  bool active = false;
};

LiveContext& live_context() {
  static LiveContext ctx;
  return ctx;
}

}  // namespace

extern "C" {

void mpi_string_free(char* s) { std::free(s); }

void mpi_cancel_all(void) { cancel_registry().cancel(); }

void mpi_cancel_reset(void) { cancel_registry().reset(); }

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
    // Which *build* of the app this is. Without it a preflight says the
    // package can be profiled and nothing about what would be measured, so
    // two captures either side of a reinstall are indistinguishable.
    if (dev != nullptr && matches == 1 && !app.empty() &&
        dev->platform == model::Platform::kAndroid) {
      std::string build_error;
      proc::Options po_build;
      po_build.timeout = std::chrono::milliseconds(
          timeout_ms > 0 ? timeout_ms : 15000);
      po_build.cancel = cancel_registry().token();
      if (!android::read_app_build_facts("adb", dev->device_id, app, po_build,
                                         build, build_error) &&
          !build_error.empty()) {
        root.set("build_facts_error", json::Value::string(build_error));
      }
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

char* mpi_boot_targets_json(void) {
  return guard([&] {
    discovery::BootOptions opts;
    opts.cancel = cancel_registry().token();
    return discovery::list_boot_targets(opts).to_json();
  });
}

char* mpi_boot_json(const char* identifier, int ready_timeout_s) {
  return guard([&] {
    json::Value out = json::Value::object();
    const std::string id = safe(identifier);
    if (id.empty()) {
      out.set("error", json::Value::string("no target identifier was given"));
      return out;
    }
    discovery::BootOptions opts;
    opts.cancel = cancel_registry().token();
    if (ready_timeout_s > 0) {
      opts.ready_timeout = std::chrono::milliseconds(ready_timeout_s * 1000);
    }
    // Resolved against the live list rather than trusted: a stale identifier
    // from a UI that has not refreshed should be an error, not an attempt to
    // boot something that is no longer there.
    const auto targets = discovery::list_boot_targets(opts);
    const discovery::BootTarget* chosen = nullptr;
    for (const auto& t : targets.targets) {
      if (t.identifier == id) chosen = &t;
    }
    if (chosen == nullptr) {
      out.set("error",
              json::Value::string("'" + id +
                                  "' is not a bootable target on this host"));
      json::Value errs = json::Value::array();
      for (const auto& e : targets.errors) errs.push_back(json::Value::string(e));
      out.set("provider_errors", std::move(errs));
      return out;
    }
    out = discovery::boot(*chosen, opts).to_json();
    out.set("target", chosen->to_json());
    return out;
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

char* mpi_session_timeline_json(const char* sessions_dir,
                                const char* session_id, int bin_count) {
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
    // Building a timeline re-reads the whole trace, and that is the one
    // expensive thing this call does. Measured on the specification's 1 GiB
    // stress fixture: the window renders correctly and costs **7.8 GB of
    // peak resident memory**, settling at 2.9 GB. That survives on a 48 GB
    // machine and would take a 16 GB one down, so it is not spent silently.
    // The limit is a default the caller raises deliberately, which is the
    // same shape as `mpi analyze --max-input-mib`.
    {
      std::error_code ec;
      const auto bytes = std::filesystem::file_size(loaded.trace_path, ec);
      constexpr std::uintmax_t kDefaultCapMiB = 256;
      if (!ec && bytes > kDefaultCapMiB * 1024 * 1024) {
        const auto mib = bytes / (1024 * 1024);
        root.set("error",
                 json::Value::string(
                     "this capture's trace is " + std::to_string(mib) +
                     " MiB, over the " + std::to_string(kDefaultCapMiB) +
                     " MiB default for building a timeline. Binning re-reads "
                     "the whole trace: on the 1 GiB stress fixture that was "
                     "measured at 7.8 GB of peak memory, which would end the "
                     "process on a 16 GB machine. Raise the cap deliberately "
                     "with `mpi timeline --max-input-mib` if this machine has "
                     "the room."));
        root.set("over_timeline_cap", json::Value::boolean(true));
        root.set("trace_mib", json::Value::integer(
                                  static_cast<std::int64_t>(mib)));
        return root;
      }
    }

    model::NormalizedTrace trace;
    ingest::ReadDiagnostics diag;
    ingest::ReadOptions opts;
    opts.cancel = cancel_registry().token();
    const auto reader = ingest::read_any(loaded.trace_path, opts, trace, diag);
    if (!reader.has_value()) {
      root.set("error",
               json::Value::string("the session's trace could not be read"));
      json::Value errs = json::Value::array();
      for (const auto& e : diag.errors) errs.push_back(json::Value::string(e));
      root.set("details", std::move(errs));
      return root;
    }
    ingest::normalize(trace, opts.cancel);

    // The bands come from running the current ruleset over the same trace, so
    // a band can never point at an interval this build would not produce.
    symbols::SymbolService symbols;
    rules::EngineOptions engine_opts;
    engine_opts.cancel = opts.cancel;
    const auto analysis = rules::analyze(trace, symbols, engine_opts);

    timeline::Options topts;
    topts.bin_count = bin_count > 0 ? bin_count : 240;
    topts.cancel = opts.cancel;
    if (trace.window_end_ns > trace.window_start_ns) {
      // Half a bin: the narrowest a band can be drawn and still be
      // unambiguous about which bin it belongs to.
      topts.min_band_ns = (trace.window_end_ns - trace.window_start_ns) /
                          (static_cast<model::TimeNs>(topts.bin_count) * 2);
    }
    root = timeline::build(trace, analysis.issues, topts).to_json();
    root.set("synthetic", json::Value::boolean(trace.synthetic));
    root.set("partial", json::Value::boolean(trace.partial));
    json::Value checks = json::Value::array();
    for (const auto& f : loaded.checksum_failures) {
      checks.push_back(json::Value::string(f));
    }
    root.set("checksum_failures", std::move(checks));
    return root;
  });
}

char* mpi_compare_json(const char* baseline_path, const char* candidate_path,
                       int min_valid_runs, double min_relative_delta,
                       double min_absolute_delta, double max_relative_spread) {
  return guard([&] {
    json::Value root = json::Value::object();
    session::RunSet baseline, candidate;
    std::string error;
    if (!session::read_run_set(safe(baseline_path), "baseline", baseline,
                               error)) {
      root.set("error", json::Value::string(error));
      return root;
    }
    if (!session::read_run_set(safe(candidate_path), "candidate", candidate,
                               error)) {
      root.set("error", json::Value::string(error));
      return root;
    }
    session::ComparisonThresholds th;
    // A threshold the caller left at zero is not a threshold of zero: it
    // means "unspecified", and the engine's own default applies. Reading it
    // as zero would clear every gate the defaults exist to hold.
    if (min_valid_runs > 0) {
      th.min_valid_runs = static_cast<std::size_t>(min_valid_runs);
    }
    if (min_relative_delta > 0.0) th.min_relative_delta = min_relative_delta;
    if (min_absolute_delta > 0.0) th.min_absolute_delta = min_absolute_delta;
    if (max_relative_spread > 0.0) th.max_relative_spread = max_relative_spread;
    root = session::compare(std::move(baseline), std::move(candidate), th)
               .to_json();
    return root;
  });
}

char* mpi_record_json(const char* sessions_dir, const char* device_id,
                      const char* app_identifier, int duration_s, int sample_hz,
                      int collect_frames, int collect_cpu, int collect_memory,
                      int reset_frame_history, int collect_scheduling,
                      int collect_heap, int timeout_ms) {
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

    // The platform's collector, chosen the same way the CLI chooses it.
    //
    // This used to refuse anything but Android, saying "the xctrace collector
    // is not wired to the session controller yet" -- which was not true: the
    // CLI has constructed it for iOS for some time, and running `mpi record`
    // against a simulator really does attach. So the desktop app could not
    // attempt something the CLI could, and told the user a reason that
    // contradicted the other front end.
    //
    // What iOS recording actually does here is fail in the collector, with
    // the collector's own account of why -- `xctrace record` attaches and
    // then never finishes. That is a provider failure and reads as one.
    std::unique_ptr<session::Collector> collector;
    if (dev->platform == model::Platform::kAndroid) {
      collector = std::make_unique<android::AdbCollector>();
    } else if (dev->platform == model::Platform::kIos) {
      collector = std::make_unique<ios::XctraceCollector>();
    }
    if (!collector) {
      out.set("error",
              json::Value::string(
                  "no collector exists for " +
                  std::string(model::to_string(dev->platform)) +
                  " in this build. The target resolved successfully, so the "
                  "blocker is the collector, not this target."));
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
    cfg.cancel = cancel_registry().token();
    cfg.duration = std::chrono::milliseconds(
        static_cast<std::int64_t>(duration_s > 0 ? duration_s : 6) * 1000);
    cfg.sample_frequency_hz = sample_hz > 0 ? sample_hz : 200;
    cfg.frames = collect_frames != 0;
    cfg.cpu_samples = collect_cpu != 0;
    cfg.memory = collect_memory != 0;
    cfg.reset_frame_history = reset_frame_history != 0;
    cfg.scheduling = collect_scheduling != 0;
    cfg.heap_dump = collect_heap != 0;
    if (cfg.heap_dump) {
      cfg.artifact_dir =
          safe(sessions_dir) + "/" + manifest.session_id + ".artifacts";
      std::error_code ec;
      std::filesystem::create_directories(cfg.artifact_dir, ec);
      if (ec) {
        out.set("error", json::Value::string("cannot create " +
                                             cfg.artifact_dir + ": " +
                                             ec.message()));
        return out;
      }
    }

    const auto capture = collector->capture(*dev, processes, cfg, trace);
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

    const auto norm = ingest::normalize(trace, cancel_registry().token());
    for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = manifest.requested_mode;
    eopts.cancel = cancel_registry().token();

    // The dump this capture just took, parsed so DET-06 runs over the same
    // session rather than needing a second command.
    heap::HeapGraph heap_graph;
    std::string heap_note;
    for (const auto& [name, path] : capture.artifacts) {
      if (name != "heap.hprof") continue;
      android::HprofLimits hlimits;
      const auto hr = android::read_hprof(path, hlimits,
                                          cancel_registry().token(), heap_graph);
      if (hr.ok && !heap_graph.objects().empty()) {
        heap_graph.gc_requested_before_dump = true;
        eopts.heap_graph = &heap_graph;
        heap_note = "heap dump parsed: " +
                    std::to_string(heap_graph.objects().size()) +
                    " object(s), " + std::to_string(heap_graph.roots().size()) +
                    " root(s)";
      } else {
        heap_note = "the heap dump was captured but could not be read (" +
                    (hr.error.empty() ? std::string("no objects found")
                                      : hr.error) +
                    "), so no reference paths are available from it";
      }
    }
    auto analysis = rules::analyze(trace, symbol_service, eopts);
    if (!heap_note.empty()) analysis.data_quality_notes.push_back(heap_note);

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
                                                trace, analysis, snap, md, js,
                                                capture.artifacts);
    if (!written.ok) {
      out.set("error", json::Value::string(written.error));
      return out;
    }
    if (!cfg.artifact_dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cfg.artifact_dir, ec);
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

char* mpi_export_session_json(const char* sessions_dir, const char* session_id,
                              const char* format, const char* out_path) {
  return guard([&] {
    json::Value out = json::Value::object();
    const std::string id = safe(session_id);
    if (!session_id_is_safe(id)) {
      out.set("error", json::Value::string("invalid session id"));
      return out;
    }
    const std::string dir = safe(sessions_dir) + "/" + id;
    const auto loaded = session::load_package(dir);
    if (!loaded.ok) {
      out.set("error", json::Value::string(loaded.error));
      return out;
    }
    const std::string fmt = safe(format);
    std::string source;
    if (fmt == "markdown" || fmt == "md") {
      source = dir + "/report.md";
    } else if (fmt == "json") {
      source = dir + "/report.json";
    } else {
      out.set("error",
              json::Value::string("format must be \"json\" or \"markdown\""));
      return out;
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
      out.set("error", json::Value::string(
                           source + " is not present in this session package"));
      return out;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const std::string target = safe(out_path);
    if (target.empty()) {
      out.set("error", json::Value::string("an output path is required"));
      return out;
    }
    std::ofstream f(target, std::ios::binary | std::ios::trunc);
    if (!f) {
      out.set("error", json::Value::string("cannot write " + target));
      return out;
    }
    f << content;
    if (!f) {
      out.set("error", json::Value::string("failed while writing " + target));
      return out;
    }
    out.set("written", json::Value::string(target));
    out.set("bytes",
            json::Value::integer(static_cast<std::int64_t>(content.size())));
    out.set("format", json::Value::string(fmt));
    // The report is copied, not regenerated, so it is exactly what was
    // recorded -- and whether that file still matches its checksum is the
    // caller's to know.
    json::Value checks = json::Value::array();
    for (const auto& c : loaded.checksum_failures) {
      checks.push_back(json::Value::string(c));
    }
    out.set("checksum_failures", std::move(checks));
    return out;
  });
}

char* mpi_suppressions_json(const char* path) {
  return guard([&] {
    json::Value out = json::Value::object();
    session::SuppressionFile file;
    const auto read = session::read_suppressions(safe(path), file);
    if (!read.ok) {
      out.set("error", json::Value::string(read.error));
      return out;
    }
    out = file.to_json();
    json::Value rejected = json::Value::array();
    for (const auto& r : read.rejected) rejected.push_back(json::Value::string(r));
    out.set("rejected", std::move(rejected));
    out.set("path", json::Value::string(safe(path)));
    return out;
  });
}

char* mpi_add_suppression_json(const char* path, const char* rule_id,
                               const char* fingerprint, const char* reason,
                               const char* expiry, const char* author,
                               const char* reference) {
  return guard([&] {
    json::Value out = json::Value::object();
    session::SuppressionEntry entry;
    entry.rule_id = safe(rule_id);
    entry.fingerprint = safe(fingerprint);
    entry.reason = safe(reason);
    entry.expiry = safe(expiry);
    entry.author = safe(author);
    entry.reference = safe(reference);
    entry.created_at = time_util::now_iso8601_utc();
    if (entry.rule_id.empty()) {
      out.set("error", json::Value::string("a rule id is required"));
      return out;
    }
    if (entry.reason.empty()) {
      out.set("error",
              json::Value::string("a reason is required: a suppression nobody "
                                  "can review is permanent by accident"));
      return out;
    }

    session::SuppressionFile file;
    const auto read = session::read_suppressions(safe(path), file);
    if (!read.ok) {
      out.set("error", json::Value::string(read.error));
      return out;
    }
    // Replacing an entry for the same rule and fingerprint rather than
    // stacking a second one, so the list cannot hold two reasons for the same
    // finding and leave a reader guessing which applied.
    for (auto it = file.entries.begin(); it != file.entries.end();) {
      if (it->rule_id == entry.rule_id && it->fingerprint == entry.fingerprint) {
        it = file.entries.erase(it);
      } else {
        ++it;
      }
    }
    file.entries.push_back(std::move(entry));
    const auto wrote = session::write_suppressions(safe(path), file);
    if (!wrote.ok) {
      out.set("error", json::Value::string(wrote.error));
      return out;
    }
    out = file.to_json();
    out.set("path", json::Value::string(safe(path)));
    return out;
  });
}

char* mpi_remove_suppression_json(const char* path, const char* rule_id,
                                  const char* fingerprint) {
  return guard([&] {
    json::Value out = json::Value::object();
    session::SuppressionFile file;
    const auto read = session::read_suppressions(safe(path), file);
    if (!read.ok) {
      out.set("error", json::Value::string(read.error));
      return out;
    }
    const std::size_t before = file.entries.size();
    for (auto it = file.entries.begin(); it != file.entries.end();) {
      if (it->rule_id == safe(rule_id) && it->fingerprint == safe(fingerprint)) {
        it = file.entries.erase(it);
      } else {
        ++it;
      }
    }
    if (file.entries.size() == before) {
      out.set("error", json::Value::string("no suppression matches that rule "
                                           "and fingerprint"));
      return out;
    }
    const auto wrote = session::write_suppressions(safe(path), file);
    if (!wrote.ok) {
      out.set("error", json::Value::string(wrote.error));
      return out;
    }
    out = file.to_json();
    out.set("path", json::Value::string(safe(path)));
    return out;
  });
}

char* mpi_reanalyze_session_json(const char* sessions_dir,
                                 const char* session_id,
                                 const char* suppressions_path) {
  return guard([&] {
    json::Value out = json::Value::object();
    const std::string id = safe(session_id);
    if (!session_id_is_safe(id)) {
      out.set("error", json::Value::string("invalid session id"));
      return out;
    }
    const std::string dir = safe(sessions_dir) + "/" + id;
    const auto loaded = session::load_package(dir);
    if (!loaded.ok) {
      out.set("error", json::Value::string(loaded.error));
      return out;
    }
    model::NormalizedTrace trace;
    ingest::ReadDiagnostics diag;
    ingest::ReadOptions ropts;
    ropts.cancel = cancel_registry().token();
    const auto reader = ingest::read_any(loaded.trace_path, ropts, trace, diag);
    if (!reader.has_value()) {
      out.set("error",
              json::Value::string("the session's trace could not be read"));
      return out;
    }
    ingest::normalize(trace, ropts.cancel);

    symbols::SymbolService symbols;
    rules::EngineOptions eopts;
    eopts.mode = trace.requested_mode;
    eopts.cancel = ropts.cancel;
    session::SuppressionFile file;
    json::Value rejected = json::Value::array();
    if (std::string(safe(suppressions_path)).size() > 0) {
      const auto read = session::read_suppressions(safe(suppressions_path), file);
      if (!read.ok) {
        out.set("error", json::Value::string(read.error));
        return out;
      }
      for (const auto& r : read.rejected) {
        rejected.push_back(json::Value::string(r));
      }
      eopts.suppressions = file.to_engine();
    }

    // A stored heap dump is used, so a re-analysis does not quietly lose
    // DET-06's evidence and report it as missing.
    heap::HeapGraph heap_graph;
    if (!loaded.heap_path.empty()) {
      android::HprofLimits hlimits;
      const auto hr = android::read_hprof(loaded.heap_path, hlimits,
                                          ropts.cancel, heap_graph);
      if (hr.ok && !heap_graph.objects().empty()) {
        heap_graph.gc_requested_before_dump = true;
        eopts.heap_graph = &heap_graph;
      }
    }

    const auto analysis = rules::analyze(trace, symbols, eopts);
    report::ReportOptions rep;
    // Suppressed findings stay in the document: that is what makes a
    // suppression auditable rather than a deletion.
    rep.include_suppressed = true;
    json::ParseError perr;
    auto doc = json::parse(report::to_json(trace, analysis, rep),
                           json::Limits{}, &perr);
    if (!doc) {
      out.set("error", json::Value::string("the re-analysis could not be "
                                           "rendered: " + perr.message));
      return out;
    }
    out = *doc;
    out.set("reanalyzed", json::Value::boolean(true));
    out.set("suppressions_applied",
            json::Value::integer(
                static_cast<std::int64_t>(eopts.suppressions.size())));
    out.set("suppressions_rejected", std::move(rejected));
    return out;
  });
}

char* mpi_analyze_trace_json(const char* trace_path) {
  return guard([&] {
    json::Value out = json::Value::object();
    model::NormalizedTrace trace;
    ingest::ReadDiagnostics diag;
    ingest::ReadOptions opts;
    opts.cancel = cancel_registry().token();
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
    ingest::normalize(trace, cancel_registry().token());

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = model::MeasurementMode::kDiagnostic;
    eopts.cancel = cancel_registry().token();
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

char* mpi_live_start(const char* sessions_dir, const char* device_id,
                     const char* app_identifier, int sample_hz,
                     int collect_frames, int collect_cpu, int collect_memory,
                     int reset_frame_history, int tick_ms, int cpu_window_ms,
                     int timeout_ms) {
  return guard([&] {
    json::Value out = json::Value::object();
    auto& ctx = live_context();
    std::lock_guard<std::mutex> ctx_lock(ctx.mutex);

    if (ctx.active && ctx.live && ctx.live->running()) {
      out.set("error",
              json::Value::string("a live session is already running; stop it "
                                  "before starting another"));
      return out;
    }

    const std::string device = safe(device_id);
    const std::string app = safe(app_identifier);
    if (device.empty() || app.empty()) {
      out.set("error", json::Value::string("device and app are both required"));
      return out;
    }

    // A previous session's stop cancelled the shared flag, and leaving it set
    // would abort this one before its first tick.
    cancel_registry().reset();

    auto svc = make_discovery(true);
    const auto po = provider_options(timeout_ms);
    // Targeted resolution rather than probe() plus a full snapshot. Those
    // enumerate every capability and every app on every device -- several
    // seconds on a host with a booted simulator, which would make "start live
    // capture" take longer than the first few seconds it is meant to show.
    const auto resolved = svc.resolve_target(device, app, po);

    if (resolved.device_ambiguous) {
      out.set("error",
              json::Value::string("device id '" + device +
                                  "' matches more than one platform"));
      return out;
    }
    if (!resolved.device_found) {
      out.set("error",
              json::Value::string("no device with id '" + device + "'"));
      return out;
    }
    const model::DeviceRef* dev = &resolved.device;
    if (!resolved.device_usable) {
      out.set("error",
              json::Value::string("device is " +
                                  std::string(model::to_string(dev->trust)) +
                                  " and cannot be used for capture"));
      return out;
    }
    if (resolved.app_ambiguous) {
      out.set("error",
              json::Value::string("'" + app +
                                  "' matches more than one entry; narrow the "
                                  "target"));
      return out;
    }
    if (!resolved.app_found) {
      out.set("error",
              json::Value::string(
                  resolved.enumeration_failed
                      ? "app enumeration failed on this device, so '" + app +
                            "' could not be resolved"
                      : "'" + app + "' was not found on this device"));
      return out;
    }
    const model::AppEntry* target = &resolved.app;
    if (target->profiling == model::ProfilingAvailability::kUnavailable) {
      out.set("error", json::Value::string("'" + app + "' cannot be profiled: " +
                                           target->profiling_reason));
      return out;
    }
    if (dev->platform != model::Platform::kAndroid) {
      // Live capture needs a collector that collects in increments. The
      // xctrace collector is wired -- a batch record uses it -- but it
      // reports `supports_streaming() == false`, because `xctrace record`
      // produces one trace bundle at the end rather than something that can
      // be read while it runs.
      //
      // The old message here said the collector was "not wired to the
      // session controller yet", which was false and contradicted what a
      // batch record does on the same device.
      out.set("error",
              json::Value::string(
                  "live capture is not available for " +
                  std::string(model::to_string(dev->platform)) +
                  " in this build: its collector does not support streaming. "
                  "`xctrace record` produces a trace bundle when it finishes "
                  "rather than events that can be read while it runs, so "
                  "there is nothing to stream. A batch capture uses the same "
                  "collector and will report what it managed; on this host "
                  "`xctrace record` attaches and then does not finish, which "
                  "it reports as a provider failure."));
      out.set("unsupported", json::Value::boolean(true));
      out.set("target_resolved", json::Value::boolean(true));
      return out;
    }

    // The app listing above already resolved the live processes, so no
    // separate revalidation round-trip is needed to start.
    auto processes = target->processes;
    if (processes.empty()) {
      out.set("error",
              json::Value::string("no live process of '" + app +
                                  "' could be resolved; start it on the device "
                                  "and retry"));
      return out;
    }

    // The discovery snapshot stored with the session records the devices seen
    // at capture time. The app listing is scoped to the target, so the
    // snapshot says so rather than implying it covered the whole host.
    model::DiscoverySnapshot snap;
    snap.taken_at = time_util::now_iso8601_utc();
    snap.devices = resolved.all_devices;
    snap.apps.push_back(*target);
    snap.provider_errors = resolved.errors;
    snap.provider_errors.push_back(
        "this snapshot was taken for a live capture: app enumeration was "
        "scoped to the selected target, so it is not a full inventory of the "
        "device");

    ctx.sessions_dir = safe(sessions_dir);
    ctx.discovery = snap;
    ctx.manifest = session::SessionManifest{};
    ctx.manifest.session_id = session::new_session_id();
    ctx.manifest.created_at = time_util::now_iso8601_utc();
    ctx.manifest.tool_version = rules::engine_version();
    ctx.manifest.requested_mode = model::MeasurementMode::kDiagnostic;
    ctx.manifest.state = session::SessionState::kRecording;
    ctx.manifest.state_transitions.push_back(
        std::string("idle -> recording (live) @ ") + ctx.manifest.created_at);

    model::NormalizedTrace trace;
    trace.session_id = ctx.manifest.session_id;
    trace.device = *dev;
    trace.requested_mode = ctx.manifest.requested_mode;
    trace.target.app = target->key;
    trace.target.processes = processes;
    trace.target.runtime_state_at_capture = target->runtime_state;
    trace.target.discovery_scope = target->visibility_scope;
    // The full capability probe is deliberately skipped for a live start: it
    // costs seconds and is what `mpi preflight` is for. The per-source results
    // the collector reports are recorded instead, and the omission is stated
    // rather than left as an empty matrix that could read as "nothing works".
    trace.ingestion_warnings.push_back(
        "no capability probe was run before this live capture: probing costs "
        "seconds and would delay the start. Run `mpi preflight` for the full "
        "matrix; the per-source results below are what this capture observed.");

    session::CaptureConfig cfg;
    cfg.sample_frequency_hz = sample_hz > 0 ? sample_hz : 200;
    cfg.frames = collect_frames != 0;
    cfg.cpu_samples = collect_cpu != 0;
    cfg.memory = collect_memory != 0;
    cfg.reset_frame_history = reset_frame_history != 0;
    cfg.tick_interval =
        std::chrono::milliseconds(tick_ms >= 100 ? tick_ms : 500);
    cfg.cpu_window =
        std::chrono::milliseconds(cpu_window_ms >= 1000 ? cpu_window_ms : 5000);
    cfg.run_until_stopped = true;

    ctx.live = std::make_unique<session::LiveSession>();
    const bool started = ctx.live->start(std::make_shared<android::AdbCollector>(),
                                         *dev, processes, cfg, std::move(trace));
    if (!started) {
      const auto s = ctx.live->snapshot();
      out.set("error", json::Value::string(s.error.empty()
                                               ? "live capture failed to start"
                                               : s.error));
      ctx.live.reset();
      return out;
    }
    ctx.active = true;
    out.set("started", json::Value::boolean(true));
    out.set("session_id", json::Value::string(ctx.manifest.session_id));
    out.set("capture_config", cfg.to_json());
    return out;
  });
}

char* mpi_live_poll_json(void) {
  return guard([] {
    auto& ctx = live_context();
    std::lock_guard<std::mutex> ctx_lock(ctx.mutex);
    if (!ctx.live) {
      json::Value out = json::Value::object();
      out.set("state", json::Value::string("idle"));
      out.set("analysis_is_preliminary", json::Value::boolean(false));
      return out;
    }
    return ctx.live->snapshot().to_json();
  });
}

int mpi_live_is_running(void) {
  auto& ctx = live_context();
  std::lock_guard<std::mutex> ctx_lock(ctx.mutex);
  return (ctx.live && ctx.live->running()) ? 1 : 0;
}

char* mpi_live_stop_json(void) {
  return guard([] {
    json::Value out = json::Value::object();
    auto& ctx = live_context();
    std::lock_guard<std::mutex> ctx_lock(ctx.mutex);
    if (!ctx.live) {
      out.set("error", json::Value::string("no live session is running"));
      return out;
    }

    ctx.live->stop();
    const auto capture = ctx.live->result();
    auto trace = ctx.live->take_trace();
    const auto final_snapshot = ctx.live->snapshot();
    ctx.live.reset();
    ctx.active = false;

    for (const auto& c : capture.source_results) trace.capabilities.upsert(c);

    json::Value srcs = json::Value::array();
    for (const auto& c : capture.source_results) srcs.push_back(c.to_json());
    out.set("source_results", std::move(srcs));
    out.set("elapsed_ms", json::Value::integer(final_snapshot.elapsed.count()));
    out.set("ticks", json::Value::integer(final_snapshot.ticks));
    out.set("collector_cost_ms",
            json::Value::integer(final_snapshot.collector_cost.count()));

    const bool any_data = !trace.frames.empty() || !trace.cpu_samples.empty() ||
                          !trace.counters.empty();
    if (!any_data) {
      // A capture that measured nothing is not written, live or not.
      out.set("session_written", json::Value::boolean(false));
      out.set("error",
              json::Value::string(capture.error.empty()
                                      ? "no source produced data"
                                      : capture.error));
      return out;
    }

    // The final analysis runs over a closed window, so it is not preliminary.
    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = ctx.manifest.requested_mode;
    auto analysis = rules::analyze(trace, symbol_service, eopts);

    report::ReportOptions rep;
    const std::string md = report::to_markdown(trace, analysis, rep);
    const std::string js = report::to_json(trace, analysis, rep);

    ctx.manifest.state = trace.partial ? session::SessionState::kPartial
                                       : session::SessionState::kCompleted;
    ctx.manifest.finalized_at = time_util::now_iso8601_utc();
    ctx.manifest.state_transitions.push_back(
        std::string("recording -> ") + session::to_string(ctx.manifest.state) +
        " @ " + ctx.manifest.finalized_at);
    ctx.manifest.synthetic = trace.synthetic;
    ctx.manifest.partial_reasons = trace.partial_reasons;

    const auto written = session::write_package(ctx.sessions_dir, ctx.manifest,
                                                trace, analysis, ctx.discovery,
                                                md, js);
    if (!written.ok) {
      out.set("session_written", json::Value::boolean(false));
      out.set("error", json::Value::string(written.error));
      return out;
    }
    out.set("session_written", json::Value::boolean(true));
    out.set("session_id", json::Value::string(ctx.manifest.session_id));
    out.set("frames",
            json::Value::integer(static_cast<std::int64_t>(trace.frames.size())));
    out.set("cpu_samples", json::Value::integer(
                               static_cast<std::int64_t>(trace.cpu_samples.size())));
    std::int64_t points = 0;
    for (const auto& c : trace.counters) {
      points += static_cast<std::int64_t>(c.points.size());
    }
    out.set("counter_points", json::Value::integer(points));
    out.set("issues", json::Value::integer(
                          static_cast<std::int64_t>(analysis.issues.size())));
    return out;
  });
}

}  /* extern "C" */
