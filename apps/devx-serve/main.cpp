// DevX -- the desktop UI for the Mobile Performance Inspector.
//
// A local web application: the C++ core serves both the API and the page, so
// there is no second toolchain, no bundler, and nothing to install alongside
// the binary. ADR-0007 records why this rather than Qt.
//
// The API is a thin projection of the same services the CLI uses. Every view
// the specification's section 13 lists is a rendering of DiscoverySnapshot,
// NormalizedTrace or AnalysisResult, all of which already serialize -- so
// DevX adds presentation and nothing else. In particular it cannot state
// anything the engine would not: the honesty rules live in the model.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/adb_collector.hpp"
#include "adapters/ios/ios_adapter.hpp"
#include "apps/devx-serve/http_server.hpp"
#include "apps/devx-serve/ui.hpp"
#include "core/discovery/discovery_service.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"
#include "core/session/session_store.hpp"
#include "core/util/process.hpp"
#include "core/util/time.hpp"

namespace mpi::devx {
namespace {

CancellationSource* g_cancel = nullptr;

extern "C" void handle_signal(int) {
  if (g_cancel) g_cancel->cancel();
}

struct Options {
  std::uint16_t port = 0;  // ephemeral by default
  bool include_simulators = true;
  bool open_browser = true;
  int timeout_ms = 20000;
  std::string sessions_dir;
};

discovery::DiscoveryService make_discovery(const Options& o,
                                           const std::string& platform) {
  discovery::DiscoveryService svc;
  if (platform.empty() || platform == "android") {
    svc.add_provider(std::make_shared<android::AdbAdapter>());
  }
  if (platform.empty() || platform == "ios") {
    auto ios_adapter = std::make_shared<ios::IosAdapter>();
    ios_adapter->set_include_simulators(o.include_simulators);
    svc.add_provider(std::move(ios_adapter));
  }
  return svc;
}

discovery::ProviderOptions provider_options(const Options& o,
                                            const CancellationToken& cancel) {
  discovery::ProviderOptions po;
  po.cancel = cancel;
  po.command_timeout_ms = o.timeout_ms;
  return po;
}

// Finds a device in a snapshot. Returns nullptr when absent, and sets
// `ambiguous` when the id matches more than one platform -- the same rule the
// CLI applies, because a shared id must never silently pick one.
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

std::vector<std::string> list_session_dirs(const std::string& root) {
  std::vector<std::string> out;
  DIR* d = ::opendir(root.c_str());
  if (!d) return out;
  while (struct dirent* e = ::readdir(d)) {
    const std::string name(e->d_name);
    if (name == "." || name == "..") continue;
    // A session package is a directory containing a manifest; anything else in
    // the sessions directory is left alone.
    struct stat st{};
    const std::string path = root + "/" + name;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    struct stat mst{};
    if (::stat((path + "/manifest.json").c_str(), &mst) != 0) continue;
    out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.rbegin(), out.rend());  // newest first, ids are date-prefixed
  return out;
}

// A session id must name a directory inside the sessions root and nothing
// else. Rejecting separators up front means no path from the request can
// escape it (spec J04).
bool session_id_is_safe(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  if (id.find('/') != std::string::npos) return false;
  if (id.find('\\') != std::string::npos) return false;
  if (id.find('\0') != std::string::npos) return false;
  if (id == "." || id == "..") return false;
  for (const char c : id) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!allowed) return false;
  }
  return true;
}

int run(const Options& opts, CancellationSource& cancel) {
  HttpServer server;

  server.route("GET", "/", [&](const Request&) {
    return Response::html(index_html(server.token(), rules::engine_version(),
                                     rules::ruleset_version()));
  });

  server.route("GET", "/api/devices", [&](const Request& req) {
    Options o = opts;
    if (req.has_param("simulators")) {
      o.include_simulators = req.param("simulators") != "0";
    }
    auto svc = make_discovery(o, req.param("platform"));
    const auto snap = svc.snapshot(provider_options(o, cancel.token()),
                                   /*include_apps=*/false);
    return Response::json(snap.to_json().dump(2));
  });

  server.route("GET", "/api/apps", [&](const Request& req) {
    const std::string device = req.param("device");
    if (device.empty()) return Response::error(400, "device is required");
    auto svc = make_discovery(opts, req.param("platform"));
    const auto snap = svc.snapshot(provider_options(opts, cancel.token()),
                                   /*include_apps=*/true);
    bool ambiguous = false;
    const model::DeviceRef* dev = find_device(snap, device, ambiguous);
    if (ambiguous) {
      return Response::error(409, "device id '" + device +
                                      "' matches more than one platform; pass "
                                      "platform= to disambiguate");
    }
    if (!dev) return Response::error(404, "no device with id '" + device + "'");

    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
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
    return Response::json(root.dump(2));
  });

  server.route("GET", "/api/preflight", [&](const Request& req) {
    const std::string device = req.param("device");
    const std::string app = req.param("app");
    auto svc = make_discovery(opts, req.param("platform"));
    const auto po = provider_options(opts, cancel.token());
    const auto caps = svc.probe(po);
    const auto snap = svc.snapshot(po, /*include_apps=*/!app.empty());

    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
    root.set("capabilities", caps.to_json());

    model::BuildProfile build;
    bool ambiguous = false;
    const model::DeviceRef* dev =
        device.empty() ? nullptr : find_device(snap, device, ambiguous);
    if (dev) {
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

    // Only an unambiguous single match becomes the target; anything else stays
    // null so the UI reports it rather than picking one.
    const model::AppEntry* target = nullptr;
    std::size_t matches = 0;
    for (const auto& a : snap.apps) {
      if (a.key.app_identifier != app) continue;
      if (dev && a.key.device_id != dev->device_id) continue;
      ++matches;
      target = &a;
    }
    root.set("target", (matches == 1 && target) ? target->to_json()
                                                : json::Value::null());
    root.set("target_match_count",
             json::Value::integer(static_cast<std::int64_t>(matches)));
    root.set("build", build.to_json());
    root.set("diagnostic_eligibility",
             model::evaluate_eligibility(build, model::MeasurementMode::kDiagnostic)
                 .to_json());
    root.set("benchmark_eligibility",
             model::evaluate_eligibility(build, model::MeasurementMode::kBenchmark)
                 .to_json());
    return Response::json(root.dump(2));
  });

  server.route("GET", "/api/rules", [&](const Request&) {
    json::Value root = json::Value::object();
    root.set("ruleset_version", json::Value::string(rules::ruleset_version()));
    root.set("engine_version", json::Value::string(rules::engine_version()));
    json::Value arr = json::Value::array();
    for (const auto& r : rules::all_rules()) arr.push_back(r->describe());
    root.set("rules", std::move(arr));
    return Response::json(root.dump(2));
  });

  server.route("GET", "/api/sessions", [&](const Request&) {
    json::Value root = json::Value::object();
    json::Value arr = json::Value::array();
    for (const auto& id : list_session_dirs(opts.sessions_dir)) {
      const auto loaded = session::load_package(opts.sessions_dir + "/" + id);
      json::Value s = json::Value::object();
      s.set("session_id", json::Value::string(id));
      if (loaded.ok) {
        s.set("state", json::Value::string(session::to_string(loaded.manifest.state)));
        s.set("created_at", json::Value::string(loaded.manifest.created_at));
        s.set("finalized_at", json::Value::string(loaded.manifest.finalized_at));
        s.set("synthetic", json::Value::boolean(loaded.manifest.synthetic));
        json::Value cf = json::Value::array();
        for (const auto& f : loaded.checksum_failures) {
          cf.push_back(json::Value::string(f));
        }
        s.set("checksum_failures", std::move(cf));
      } else {
        // A package that will not load is listed with its error rather than
        // omitted, so a half-written session is visible.
        s.set("state", json::Value::string("unreadable"));
        s.set("error", json::Value::string(loaded.error));
        s.set("synthetic", json::Value::null());
      }
      arr.push_back(std::move(s));
    }
    root.set("sessions", std::move(arr));
    root.set("sessions_dir", json::Value::string(opts.sessions_dir));
    return Response::json(root.dump(2));
  });

  server.route("GET", "/api/session", [&](const Request& req) {
    const std::string id = req.param("id");
    if (!session_id_is_safe(id)) {
      return Response::error(400, "invalid session id");
    }
    const std::string dir = opts.sessions_dir + "/" + id;
    const auto loaded = session::load_package(dir);
    if (!loaded.ok) return Response::error(404, loaded.error);

    // The stored report is served as-is: re-analysing here could disagree with
    // what the session recorded, and the session is the artefact of record.
    json::ParseError perr;
    auto report = json::parse_file(dir + "/report.json", json::Limits{}, &perr);
    if (!report) {
      return Response::error(500, "session report is unreadable: " + perr.message);
    }
    json::Value root = *report;
    json::Value checks = json::Value::array();
    for (const auto& f : loaded.checksum_failures) {
      checks.push_back(json::Value::string(f));
    }
    root.set("checksum_failures", std::move(checks));
    root.set("manifest_state",
             json::Value::string(session::to_string(loaded.manifest.state)));
    return Response::json(root.dump());
  });

  server.route("POST", "/api/record", [&](const Request& req) {
    json::ParseError perr;
    auto body = json::parse(req.body, json::Limits{}, &perr);
    if (!body || !body->is_object()) {
      return Response::error(400, "body must be a JSON object: " + perr.message);
    }
    const auto str = [&](const char* k) {
      const json::Value* v = body->find(k);
      return v && v->is_string() ? v->as_string() : std::string();
    };
    const auto num = [&](const char* k, std::int64_t fallback) {
      const json::Value* v = body->find(k);
      return v && v->is_number() ? v->as_int() : fallback;
    };
    const auto flag = [&](const char* k, bool fallback) {
      const json::Value* v = body->find(k);
      return v && v->is_bool() ? v->as_bool() : fallback;
    };

    const std::string device = str("device");
    const std::string app = str("app");
    if (device.empty() || app.empty()) {
      return Response::error(400, "device and app are both required");
    }

    auto svc = make_discovery(opts, "");
    const auto po = provider_options(opts, cancel.token());
    const auto caps = svc.probe(po);
    const auto snap = svc.snapshot(po, /*include_apps=*/true);

    bool ambiguous = false;
    const model::DeviceRef* dev = find_device(snap, device, ambiguous);
    if (ambiguous) return Response::error(409, "ambiguous device id");
    if (!dev) return Response::error(404, "no device with id '" + device + "'");
    if (!dev->usable_for_capture()) {
      return Response::error(409, "device is " +
                                      std::string(model::to_string(dev->trust)) +
                                      " and cannot be used for capture");
    }

    const model::AppEntry* target = nullptr;
    std::size_t matches = 0;
    for (const auto& a : snap.apps) {
      if (a.key.app_identifier != app) continue;
      if (a.key.device_id != device) continue;
      ++matches;
      target = &a;
    }
    if (matches == 0) {
      return Response::error(404, "'" + app + "' was not found on this device");
    }
    if (matches > 1) {
      return Response::error(409, "'" + app +
                                      "' matches more than one entry on this "
                                      "device; narrow the target");
    }
    if (target->profiling == model::ProfilingAvailability::kUnavailable) {
      return Response::error(409, "'" + app + "' cannot be profiled: " +
                                      target->profiling_reason);
    }

    // Only Android has a collector. iOS says so rather than producing an
    // empty session, which is the same refusal the CLI makes.
    if (dev->platform != model::Platform::kAndroid) {
      json::Value out = json::Value::object();
      out.set("error",
              json::Value::string(
                  "live capture is not implemented for " +
                  std::string(model::to_string(dev->platform)) +
                  " in this build: the xctrace collector is not wired to the "
                  "session controller yet. The target resolved successfully, "
                  "so the blocker is the collector, not this target."));
      out.set("target_resolved", json::Value::boolean(true));
      out.set("source_results", json::Value::array());
      return Response::json(out.dump(2), 501);
    }

    const auto reval =
        svc.revalidate(*dev, target->key, target->processes, po);
    auto processes = reval.processes.empty() ? target->processes : reval.processes;
    if (processes.empty()) {
      return Response::error(409,
                             "no live process of '" + app +
                                 "' could be resolved; start it on the device "
                                 "and retry");
    }

    session::SessionManifest manifest;
    manifest.session_id = session::new_session_id();
    manifest.created_at = time_util::now_iso8601_utc();
    manifest.tool_version = rules::engine_version();
    manifest.requested_mode = model::MeasurementMode::kDiagnostic;
    manifest.state = session::SessionState::kRecording;
    manifest.state_transitions.push_back(
        std::string("idle -> recording @ ") + manifest.created_at);

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
    cfg.cancel = cancel.token();
    cfg.duration = std::chrono::milliseconds(
        std::max<std::int64_t>(1, num("duration_s", 6)) * 1000);
    cfg.sample_frequency_hz =
        static_cast<int>(std::max<std::int64_t>(1, num("sample_hz", 200)));
    cfg.frames = flag("frames", true);
    cfg.cpu_samples = flag("cpu", true);
    cfg.memory = flag("memory", true);
    cfg.reset_frame_history = flag("reset_frame_history", true);

    android::AdbCollector collector;
    const auto capture = collector.capture(*dev, processes, cfg, trace);
    for (const auto& c : capture.source_results) trace.capabilities.upsert(c);

    json::Value out = json::Value::object();
    json::Value srcs = json::Value::array();
    for (const auto& c : capture.source_results) srcs.push_back(c.to_json());
    out.set("source_results", std::move(srcs));
    out.set("elapsed_ms", json::Value::integer(capture.elapsed.count()));
    out.set("capture_config", cfg.to_json());

    if (!capture.started) {
      out.set("error", json::Value::string(capture.error));
      return Response::json(out.dump(2), 500);
    }
    if (!capture.any_data) {
      // A capture that measured nothing is not written. Saving it would be the
      // capture-shaped-but-empty artefact spec section 0.5 forbids.
      out.set("error", json::Value::string(capture.error));
      out.set("session_written", json::Value::boolean(false));
      return Response::json(out.dump(2), 200);
    }

    const auto norm = ingest::normalize(trace, cancel.token());
    for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);

    symbols::SymbolService symbol_service;
    rules::EngineOptions eopts;
    eopts.mode = manifest.requested_mode;
    eopts.cancel = cancel.token();
    auto analysis = rules::analyze(trace, symbol_service, eopts);

    report::ReportOptions rep;
    const std::string md = report::to_markdown(trace, analysis, rep);
    const std::string js = report::to_json(trace, analysis, rep);

    manifest.state = trace.partial ? session::SessionState::kPartial
                                   : session::SessionState::kCompleted;
    manifest.finalized_at = time_util::now_iso8601_utc();
    manifest.state_transitions.push_back(
        std::string("recording -> ") + session::to_string(manifest.state) + " @ " +
        manifest.finalized_at);
    manifest.synthetic = trace.synthetic;
    manifest.partial_reasons = trace.partial_reasons;

    const auto written = session::write_package(opts.sessions_dir, manifest, trace,
                                                analysis, snap, md, js);
    if (!written.ok) {
      out.set("error", json::Value::string(written.error));
      return Response::json(out.dump(2), 500);
    }
    out.set("session_written", json::Value::boolean(true));
    out.set("session_id", json::Value::string(manifest.session_id));
    out.set("frames",
            json::Value::integer(static_cast<std::int64_t>(trace.frames.size())));
    out.set("cpu_samples",
            json::Value::integer(static_cast<std::int64_t>(trace.cpu_samples.size())));
    out.set("counters",
            json::Value::integer(static_cast<std::int64_t>(trace.counters.size())));
    out.set("issues",
            json::Value::integer(static_cast<std::int64_t>(analysis.issues.size())));
    return Response::json(out.dump(2));
  });

  std::string error;
  if (!server.listen(opts.port, &error)) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) +
                          "/?token=" + server.token();
  // Flushed explicitly: stdout is fully buffered when redirected, and the URL
  // carries the token, so withholding it until exit makes the server unusable
  // for anyone piping the output to a file or a wrapper script.
  std::cout << "DevX is serving on " << url << "\n"
            << "  bound to 127.0.0.1 only; every request must carry the token "
               "above\n"
            << "  sessions directory: " << opts.sessions_dir << "\n"
            << "  press Ctrl-C to stop\n"
            << std::flush;

  if (opts.open_browser) {
    // Opening a browser is a convenience; failing to is not an error.
    const auto r = proc::run({"open", url});
    if (!r.ok()) {
      std::cout << "  (could not open a browser automatically; paste the URL)\n"
                << std::flush;
    }
  }

  server.serve(cancel.token());
  std::cout << "DevX stopped.\n" << std::flush;
  return 0;
}

const char* kUsage = R"(devx-serve -- DevX's views over loopback HTTP

For the macOS desktop application, use DevX.app instead. This exists for a host
without SwiftUI, or a session reached over SSH.

USAGE
  devx-serve [options]

OPTIONS
  --port <n>          Port to bind on 127.0.0.1 (default: an ephemeral port).
  --no-simulators     Exclude iOS simulators from discovery.
  --no-open           Do not open a browser automatically.
  --sessions-dir <p>  Where session packages live (default ~/.mpi/sessions).
  --timeout-ms <n>    Per-command device tooling timeout.
  -h, --help          This text.

SECURITY
  The listener binds to 127.0.0.1 and cannot be configured otherwise, and every
  request must present the token printed at startup. Loopback alone is not
  authentication: any local process could otherwise reach the API.
)";

}  // namespace
}  // namespace mpi::devx

int main(int argc, char** argv) {
  using namespace mpi;
  using namespace mpi::devx;

  Options opts;
  const char* home = std::getenv("HOME");
  opts.sessions_dir =
      (home && *home ? std::string(home) : std::string(".")) + "/.mpi/sessions";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: " << what << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      std::cout << kUsage;
      return 0;
    } else if (a == "--port") {
      const int p = std::atoi(next("--port").c_str());
      if (p < 1 || p > 65535) {
        std::cerr << "error: --port must be between 1 and 65535\n";
        return 2;
      }
      opts.port = static_cast<std::uint16_t>(p);
    } else if (a == "--no-simulators") {
      opts.include_simulators = false;
    } else if (a == "--no-open") {
      opts.open_browser = false;
    } else if (a == "--sessions-dir") {
      opts.sessions_dir = next("--sessions-dir");
    } else if (a == "--timeout-ms") {
      opts.timeout_ms = std::atoi(next("--timeout-ms").c_str());
      if (opts.timeout_ms <= 0) {
        std::cerr << "error: --timeout-ms must be positive\n";
        return 2;
      }
    } else {
      std::cerr << "error: unknown option '" << a << "'\n\n" << kUsage;
      return 2;
    }
  }

  CancellationSource cancel;
  g_cancel = &cancel;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);  // a client that hangs up must not kill us

  return run(opts, cancel);
}
