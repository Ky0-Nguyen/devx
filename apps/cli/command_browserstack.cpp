// `mpi browserstack` -- real devices in BrowserStack's cloud.
//
//   status                           whether the credentials work: the App Automate plan
//   devices                          the real devices the account can use
//   builds [<build_id>]              recent App Automate builds, or one build's sessions
//   upload <file> [--product P]      an .apk/.aab/.ipa; app-automate (default) or app-live
//   start <bs://app> <device> <os-version> [--platform android|ios]
//         [--network P] [--gps lat,lng] [--timezone T] [--language l] [--locale L]
//         [--orientation portrait|landscape] [--biometric] [--camera-injection]
//         [--profiling] [--local]
//                                    an App Automate session DevX drives; prints its id
//   screenshot <session> --out F     the screen, as a PNG
//   tap <session> <x> <y> | swipe <session> <x1> <y1> <x2> <y2>
//   type <session> <text> | key <session> back|home|enter
//   stop <session> [--import-profiling]
//                                    ends it; with the flag, imports its App Profiling
//   import <session_id> [--build B]  App Profiling of one session, as a DevX session
//
// Coordinates are screenshot pixels. Credentials come from
// BROWSERSTACK_USERNAME / BROWSERSTACK_ACCESS_KEY or the Keychain entry DevX's
// Emulator tab saves. See docs/browserstack.md.
#include <fstream>
#include <iostream>

#include "adapters/browserstack/automate.hpp"
#include "adapters/browserstack/browserstack.hpp"
#include "adapters/browserstack/profiling.hpp"
#include "apps/cli/cli.hpp"

namespace mpi::cli {
namespace {

std::string arg(const Invocation& inv, std::size_t i) {
  return i < inv.positional.size() ? inv.positional[i] : std::string();
}

int int_arg(const Invocation& inv, std::size_t i) { return std::atoi(arg(inv, i).c_str()); }

bool is_id(const std::string& s) {
  return !s.empty() && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") ==
                           std::string::npos;
}

// A reply printed as BrowserStack sent it; the exit code says whether it worked.
ExitCode print_reply(const browserstack::Reply& r) {
  std::cout << r.to_json().dump(2) << "\n";
  return r.ok ? ExitCode::kOk : ExitCode::kCollectionError;
}

ExitCode print_import(const Invocation& inv, const std::string& session,
                      const browserstack::ProfilingImport& result) {
  if (inv.global.json) {
    std::cout << result.to_json().dump(2) << "\n";
  } else if (result.ok) {
    std::cout << "imported BrowserStack session " << session << " as DevX session "
              << result.devx_session_id << " (" << result.series << " series)\n"
              << "  " << result.package_dir << "\n";
    for (const auto& n : result.notes) std::cout << "  note: " << n << "\n";
  } else {
    std::cerr << "error: " << result.error << "\n";
    for (const auto& n : result.notes) std::cerr << "  note: " << n << "\n";
  }
  if (result.ok) return ExitCode::kOk;
  return inv.global.cancel.cancelled() ? ExitCode::kCancelled : ExitCode::kCollectionError;
}

ExitCode usage() {
  std::cerr << "usage: mpi browserstack status | devices | builds [<build>] | upload <file> |\n"
               "       start <bs://app> <device> <os-version> [options] |\n"
               "       screenshot <session> --out <file.png> | tap <session> <x> <y> |\n"
               "       swipe <session> <x1> <y1> <x2> <y2> | type <session> <text> |\n"
               "       key <session> back|home|enter | stop <session> [--import-profiling] |\n"
               "       import <session> [--build <build>]\n";
  return ExitCode::kUsage;
}

}  // namespace

ExitCode cmd_browserstack(const Invocation& inv) {
  const std::string sub = arg(inv, 0);
  static const char* kSubs[] = {"status", "devices", "builds", "upload", "start", "screenshot",
                                "tap",    "swipe",   "type",   "key",    "stop",  "import"};
  bool known = false;
  for (const char* s : kSubs) known = known || sub == s;
  if (!known) return usage();

  const auto c = browserstack::find_credentials();
  if (!c) {
    std::cerr << "error: no BrowserStack credentials: set BROWSERSTACK_USERNAME and "
                 "BROWSERSTACK_ACCESS_KEY, or save them from DevX's Emulator tab\n";
    return ExitCode::kNotFound;
  }

  if (sub == "status") return print_reply(browserstack::get(*c, "app-automate/plan.json"));
  if (sub == "devices") return print_reply(browserstack::get(*c, "app-automate/devices.json"));
  if (sub == "builds") {
    const std::string build = arg(inv, 1);
    if (!build.empty() && !is_id(build)) {
      std::cerr << "error: not a build id: " << build << "\n";
      return ExitCode::kUsage;
    }
    return print_reply(browserstack::get(
        *c, build.empty() ? "app-automate/builds.json?limit=20"
                          : "app-automate/builds/" + build + "/sessions.json"));
  }
  if (sub == "upload") {
    if (arg(inv, 1).empty()) return usage();
    return print_reply(
        browserstack::upload(*c, inv.flag("product", "app-automate"), arg(inv, 1)));
  }

  if (sub == "start") {
    browserstack::AutomateSpec spec;
    spec.app_url = arg(inv, 1);
    spec.device = arg(inv, 2);
    spec.os_version = arg(inv, 3);
    spec.platform = inv.global.platform.empty() ? "android" : inv.global.platform;
    spec.network_profile = inv.flag("network");
    spec.gps_location = inv.flag("gps");
    spec.timezone = inv.flag("timezone");
    spec.language = inv.flag("language");
    spec.locale = inv.flag("locale");
    spec.orientation = inv.flag("orientation");
    spec.biometric = inv.has_flag("biometric");
    spec.camera_injection = inv.has_flag("camera-injection");
    spec.app_profiling = inv.has_flag("profiling");
    spec.local = inv.has_flag("local");
    if (spec.local) spec.local_identifier = "devx-cli";
    const auto r = browserstack::start_session(*c, spec);
    if (inv.global.json || !r.ok) {
      (r.ok ? std::cout : std::cerr) << r.to_json().dump(2) << "\n";
    } else {
      std::cout << r.session_id << "\n";
    }
    return r.ok ? ExitCode::kOk : ExitCode::kCollectionError;
  }

  const std::string session = arg(inv, 1);
  if (session.empty()) return usage();

  if (sub == "screenshot") {
    const std::string out = inv.flag("out");
    if (out.empty()) return usage();
    const auto s = browserstack::screenshot(*c, session);
    if (!s.ok) {
      std::cerr << "error: " << s.error << "\n";
      return ExitCode::kCollectionError;
    }
    std::ofstream f(out, std::ios::binary);
    f.write(s.png.data(), static_cast<std::streamsize>(s.png.size()));
    if (!f) {
      std::cerr << "error: could not write " << out << "\n";
      return ExitCode::kCollectionError;
    }
    std::cout << out << " (" << s.width << "x" << s.height << ")\n";
    return ExitCode::kOk;
  }
  if (sub == "tap") return print_reply(browserstack::tap(*c, session, int_arg(inv, 2), int_arg(inv, 3)));
  if (sub == "swipe") {
    return print_reply(browserstack::swipe(*c, session, int_arg(inv, 2), int_arg(inv, 3),
                                           int_arg(inv, 4), int_arg(inv, 5)));
  }
  if (sub == "type") return print_reply(browserstack::type_text(*c, session, arg(inv, 2)));
  if (sub == "key") return print_reply(browserstack::press(*c, session, arg(inv, 2)));
  if (sub == "stop") {
    const bool import = inv.has_flag("import-profiling");
    const auto r = browserstack::stop_session(*c, session, import ? inv.global.sessions_dir : "",
                                              inv.global.cancel);
    std::cout << r.to_json().dump(2) << "\n";
    if (!r.ok) return ExitCode::kCollectionError;
    return import && !r.imported ? ExitCode::kCollectionError : ExitCode::kOk;
  }

  // import
  browserstack::ProfilingRequest req;
  req.session_id = session;
  req.build_id = inv.flag("build");
  req.sessions_dir = inv.global.sessions_dir;
  if (!is_id(req.session_id)) return usage();
  return print_import(inv, session, browserstack::import_profiling(*c, req, inv.global.cancel));
}

}  // namespace mpi::cli
