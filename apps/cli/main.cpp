#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/ios/ios_adapter.hpp"
#include "apps/cli/cli.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"

namespace mpi::cli {
namespace {

// Signal-driven cancellation, so Ctrl-C unwinds cleanly and frees buffers and
// workers rather than leaving an orphaned collector (spec section 14, J11).
CancellationSource* g_cancel = nullptr;

extern "C" void handle_signal(int) {
  if (g_cancel) g_cancel->cancel();
}

const char* kUsage = R"(mpi -- Mobile Performance Inspector

USAGE
  mpi <command> [options]

COMMANDS
  devices                       List connected Android and iOS devices.
  boot                          Start a simulator or emulator and wait for it
                                (--list, --device <avd|udid>,
                                --ready-timeout-s). "started" and "ready" are
                                reported separately.
  apps                          List apps on a device (--running / --installed).
  preflight                     Probe capabilities for a device + app target.
  record                        Record a capture session.
  analyze <session|trace>       Analyze a session package or a trace file.
  timeline <session|trace>      Draw the capture's tracks (--bins, --json).
                                A blank column means NOT MEASURED, never zero.
                                Binning re-reads the whole trace: measured at
                                7.8 GB of peak memory on the 1 GiB stress
                                fixture, so DevX refuses above 256 MiB and
                                --max-input-mib is how you say you have the
                                room.
  compare <baseline> <cand>     Compare two run-set JSON files.
  export <session>              Re-export a session (--format json|markdown).
  rules                         Describe every detector and its thresholds.

GLOBAL OPTIONS
  --device <id>                 Device id (adb serial, or CoreDevice/sim UDID).
  --app <identifier>            Android package name or iOS bundle id.
  --platform <android|ios>      Disambiguate when a device id is ambiguous.
  --json                        Machine-readable output.
  --ci                          Never prompt; ambiguity is an error.
  --no-simulators               Exclude iOS simulators from discovery.
  --timeout-ms <n>              Per-command device tooling timeout.
  --sessions-dir <path>         Where session packages live.
  --max-input-mib <n>           Cap on a single input file (default 2048).

RECORD OPTIONS (Android)
  --duration-s <n>              Capture duration in seconds (default 5).
  --sample-hz <n>               CPU sampling frequency (default 200).
  --no-frames / --no-cpu / --no-memory
                                Disable an individual collector source.
  --no-frame-reset              Keep the frame history the platform already
                                holds instead of resetting it first.
  --suppressions <file>         (analyze) A project's suppression list, shared
                                with the desktop app. An entry with no reason
                                is refused, and an expired one is reported and
                                NOT applied.
  --heap-dump <file>            (analyze) Read reference paths from an HPROF
                                heap dump, for DET-06.
  --heap                        (record) Take a heap dump after the capture, for
                                DET-06's reference paths. Pauses the app and
                                writes tens of megabytes; the note it prints
                                says so before it runs.
  --live                        Stream the capture: print counts as they
                                arrive instead of reporting only at the end.
                                Live findings are always preliminary.
  --tick-ms <n>                 Streaming cadence (default 750). Shorter means
                                fresher numbers and more collector overhead.
  --import <trace-file>         Build a session from an existing trace instead.
  --quiet                       Suppress warnings on stderr.
  -h, --help                    This text.

EXIT CODES
  0  ok                         4  inconclusive        7  ambiguous target
  2  usage error                5  collection error    8  cancelled
  3  regression detected        6  unsupported         9  not found

NOTES
  Selecting an app is done by package name or bundle id. A PID is never
  required. Selecting an identifier does not bypass any platform restriction:
  trust prompts, developer mode, signing, and manifest flags still apply.
)";

struct ParseResult {
  bool ok = false;
  Invocation inv;
  std::string error;
};

// Flags that consume the following token as their value.
//
// This list must contain every flag any subcommand reads with `inv.flag()`.
// A flag missing here is parsed as a boolean and its intended value silently
// becomes a positional argument, so `tools/smoke-test.sh` asserts the two
// lists agree.
bool needs_value(const std::string& flag) {
  static const char* kWithValue[] = {
      "--app",
      "--bins",
      "--ready-timeout-s",
      "--heap-dump",
      "--bundle-id",
      "--device",
      "--duration-s",
      "--format",
      "--import",
      "--label",
      "--max-input-mib",
      "--max-markers",
      "--max-spread",
      "--metro-port",
      "--screenshot-dir",
      "--seconds",
      "--min-absolute-delta",
      "--min-relative-delta",
      "--min-runs",
      "--mode",
      "--native-build-id",
      "--out",
      "--platform",
      "--preset",
      "--r8-map",
      "--sample-hz",
      "--sdk-port",
      "--tick-ms",
      "--wait-for-app-s",
      "--search",
      "--launch-class",
      "--sessions-dir",
      "--source-map",
      "--source-revision",
      "--source-root",
      "--suppress",
      "--suppressions",
      "--suppress-expiry",
      "--suppress-reason",
      "--threshold",
      "--timeout-ms"};
  for (const char* f : kWithValue) {
    if (flag == f) return true;
  }
  return false;
}

// Flags that take no value.
//
// Together with `needs_value` this is the whole vocabulary: anything else is
// refused. Silently ignoring an unknown flag let a typo change what was
// measured without saying so -- `--duration 14` instead of `--duration-s 14`
// recorded the default 5 s and reported success, which is precisely the kind
// of quiet substitution this tool exists not to do.
bool is_boolean_flag(const std::string& flag) {
  static const char* kBoolean[] = {
      "--ci",           "--include-events", "--include-source-paths",
      "--installed",    "--json",           "--launch",
      "--list",
      "--live",
      "--heap",
      "--no-cpu",       "--no-frame-reset", "--no-frames",
      "--no-memory",    "--no-simulators",  "--profileable",
      "--quiet",        "--running",        "--scheduling",
      "--sdk",          "--source-dirty",
      "--targets",      "--redux",          "--redux-values",
      "--screenshot",
      "--synthetic",    "--help"};
  for (const char* f : kBoolean) {
    if (flag == f) return true;
  }
  return false;
}

ParseResult parse(int argc, char** argv) {
  ParseResult r;
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    r.error = "no command given";
    return r;
  }
  std::size_t i = 0;
  if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    r.inv.command = "help";
    r.ok = true;
    return r;
  }
  if (args[0].rfind("-", 0) == 0) {
    r.error = "expected a command, got the option '" + args[0] + "'";
    return r;
  }
  r.inv.command = args[0];
  ++i;

  for (; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--", 0) != 0) {
      if (a == "-h") {
        r.inv.command = "help";
        r.ok = true;
        return r;
      }
      r.inv.positional.push_back(a);
      continue;
    }
    std::string name = a;
    std::string value;
    bool have_value = false;
    const std::size_t eq = a.find('=');
    if (eq != std::string::npos) {
      name = a.substr(0, eq);
      value = a.substr(eq + 1);
      have_value = true;
    } else if (needs_value(name)) {
      if (i + 1 >= args.size()) {
        r.error = name + " requires a value";
        return r;
      }
      value = args[++i];
      have_value = true;
    }

    if (name == "--json") {
      r.inv.global.json = true;
    } else if (name == "--ci") {
      r.inv.global.ci = true;
    } else if (name == "--quiet") {
      r.inv.global.quiet = true;
    } else if (name == "--no-simulators") {
      r.inv.global.include_simulators = false;
    } else if (name == "--device") {
      r.inv.global.device = value;
    } else if (name == "--app") {
      r.inv.global.app = value;
    } else if (name == "--platform") {
      if (value != "android" && value != "ios") {
        r.error = "--platform must be 'android' or 'ios'";
        return r;
      }
      r.inv.global.platform = value;
    } else if (name == "--timeout-ms") {
      r.inv.global.timeout_ms = std::atoi(value.c_str());
      if (r.inv.global.timeout_ms <= 0) {
        r.error = "--timeout-ms must be a positive integer";
        return r;
      }
    } else if (name == "--sessions-dir") {
      r.inv.global.sessions_dir = value;
    } else if (name == "--help") {
      r.inv.command = "help";
      r.ok = true;
      return r;
    } else if (!needs_value(name) && !is_boolean_flag(name)) {
      r.error = "unknown flag '" + name +
                "'. Run `mpi help` for the flags this command accepts; a "
                "flag that was ignored would have changed what was measured "
                "without saying so";
      return r;
    } else {
      r.inv.flags.emplace_back(name.substr(2), have_value ? value : "true");
    }
  }

  if (r.inv.global.sessions_dir.empty()) {
    const char* home = std::getenv("HOME");
    r.inv.global.sessions_dir =
        (home && *home ? std::string(home) : std::string(".")) +
        "/.mpi/sessions";
  }
  r.ok = true;
  return r;
}

}  // namespace

int to_int(ExitCode c) { return static_cast<int>(c); }

const char* describe(ExitCode c) {
  switch (c) {
    case ExitCode::kOk: return "ok";
    case ExitCode::kUsage: return "usage error";
    case ExitCode::kRegressionDetected: return "regression detected";
    case ExitCode::kInconclusive: return "inconclusive";
    case ExitCode::kCollectionError: return "collection error";
    case ExitCode::kUnsupportedOperation: return "unsupported operation";
    case ExitCode::kAmbiguousTarget: return "ambiguous target";
    case ExitCode::kCancelled: return "cancelled";
    case ExitCode::kNotFound: return "not found";
  }
  return "unknown";
}

std::string Invocation::flag(const std::string& name,
                             const std::string& fallback) const {
  for (const auto& f : flags) {
    if (f.first == name) return f.second;
  }
  return fallback;
}

bool Invocation::has_flag(const std::string& name) const {
  for (const auto& f : flags) {
    if (f.first == name) return true;
  }
  return false;
}

discovery::ProviderOptions provider_options(const GlobalOptions& opts) {
  discovery::ProviderOptions po;
  po.cancel = opts.cancel;
  po.command_timeout_ms = opts.timeout_ms;
  return po;
}

discovery::DiscoveryService make_discovery(const GlobalOptions& opts) {
  discovery::DiscoveryService svc;
  // Both platforms are registered unless the caller narrows the scope. The
  // specification is explicit that this must not quietly become Android-only.
  if (opts.platform.empty() || opts.platform == "android") {
    svc.add_provider(std::make_shared<android::AdbAdapter>());
  }
  if (opts.platform.empty() || opts.platform == "ios") {
    auto ios_adapter = std::make_shared<ios::IosAdapter>();
    ios_adapter->set_include_simulators(opts.include_simulators);
    svc.add_provider(std::move(ios_adapter));
  }
  return svc;
}

void print_json(const json::Value& v) { std::cout << v.dump(2) << "\n"; }

void warn(const std::string& msg) { std::cerr << "warning: " << msg << "\n"; }

bool resolve_device(const Invocation& inv, const model::DiscoverySnapshot& snap,
                    model::DeviceRef& out, ExitCode& code) {
  const std::string& want = inv.global.device;
  if (!want.empty()) {
    std::vector<const model::DeviceRef*> hits;
    for (const auto& d : snap.devices) {
      if (d.matches_id(want)) hits.push_back(&d);
    }
    if (hits.empty()) {
      std::cerr << "error: no device with id '" << want << "'. Run `mpi devices`.\n";
      code = ExitCode::kNotFound;
      return false;
    }
    if (hits.size() > 1) {
      // The same id on two platforms stays separate (spec A15).
      std::cerr << "error: device id '" << want
                << "' matches more than one device; add --platform.\n";
      for (const auto* d : hits) {
        std::cerr << "  - " << model::to_string(d->platform) << " "
                  << d->display_name << "\n";
      }
      code = ExitCode::kAmbiguousTarget;
      return false;
    }
    out = *hits.front();
    if (!out.usable_for_capture()) {
      std::cerr << "error: device '" << want << "' is "
                << model::to_string(out.trust) << " and cannot be used.\n";
      // "offline" is true and useless on its own. The probe costs about a
      // tenth of a second, and this is exactly the moment worth spending it:
      // the operator is standing there wondering what to plug in.
      discovery::ProviderOptions popts;
      popts.cancel = inv.global.cancel;
      for (const std::string& line :
           ios::explain_unusable_device(out, popts, /*probe=*/true)) {
        std::cerr << "  - " << line << "\n";
      }
      code = ExitCode::kCollectionError;
      return false;
    }
    return true;
  }

  discovery::DiscoveryService svc;
  const auto sel = svc.select_device(snap);
  if (sel.candidates.empty()) {
    std::cerr << "error: " << sel.note << ". Run `mpi devices` for details.\n";
    code = ExitCode::kNotFound;
    return false;
  }
  if (sel.candidates.size() > 1 || inv.global.ci) {
    // In CI mode even a single candidate must be named explicitly, so a
    // pipeline cannot silently change target when the lab changes.
    if (sel.candidates.size() > 1) {
      std::cerr << "error: " << sel.note << ". Choose one with --device:\n";
    } else {
      std::cerr << "error: --ci requires an explicit --device. Available:\n";
    }
    for (const auto& d : sel.candidates) {
      std::cerr << "  --device " << d.device_id << "   ("
                << model::to_string(d.platform) << ", " << d.display_name << ", "
                << model::to_string(d.form) << ")\n";
    }
    code = ExitCode::kAmbiguousTarget;
    return false;
  }
  out = *sel.preselected;
  if (!inv.global.quiet) {
    std::cerr << "note: " << sel.note << " (" << out.device_id << ")\n";
  }
  return true;
}

}  // namespace mpi::cli

int main(int argc, char** argv) {
  using namespace mpi::cli;

  mpi::CancellationSource cancel;
  g_cancel = &cancel;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  auto parsed = parse(argc, argv);
  if (!parsed.ok) {
    std::cerr << "error: " << parsed.error << "\n\n" << kUsage;
    return to_int(ExitCode::kUsage);
  }
  parsed.inv.global.cancel = cancel.token();

  if (parsed.inv.command == "help") {
    std::cout << kUsage;
    return to_int(ExitCode::kOk);
  }

  ExitCode code = ExitCode::kUsage;
  const std::string& cmd = parsed.inv.command;
  if (cmd == "boot") {
    code = cmd_boot(parsed.inv);
  } else if (cmd == "devices") {
    code = cmd_devices(parsed.inv);
  } else if (cmd == "apps") {
    code = cmd_apps(parsed.inv);
  } else if (cmd == "preflight") {
    code = cmd_preflight(parsed.inv);
  } else if (cmd == "record") {
    code = cmd_record(parsed.inv);
  } else if (cmd == "analyze") {
    code = cmd_analyze(parsed.inv);
  } else if (cmd == "timeline") {
    code = cmd_timeline(parsed.inv);
  } else if (cmd == "compare") {
    code = cmd_compare(parsed.inv);
  } else if (cmd == "export") {
    code = cmd_export(parsed.inv);
  } else if (cmd == "rules") {
    code = cmd_rules(parsed.inv);
  } else if (cmd == "inspect") {
    return to_int(cmd_inspect(parsed.inv));
  } else if (cmd == "sdk-bridge") {
    code = cmd_sdk_bridge(parsed.inv);
  } else if (cmd == "version") {
    std::cout << "mpi " << mpi::rules::engine_version() << " (ruleset "
              << mpi::rules::ruleset_version() << ")\n";
    code = ExitCode::kOk;
  } else {
    std::cerr << "error: unknown command '" << cmd << "'\n\n" << kUsage;
    return to_int(ExitCode::kUsage);
  }

  if (cancel.cancelled() && code == ExitCode::kOk) code = ExitCode::kCancelled;
  return to_int(code);
}
