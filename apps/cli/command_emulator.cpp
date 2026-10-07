// `mpi emulator` -- Android emulators without Android Studio.
//
//   sdk                               where the SDK is, what is installed, what runs
//   catalog                           what can be installed (stable channel, this Mac's ABI)
//   license <id> [--accept]           show a license; --accept records that you accept it
//   install <package-path>            download, verify and unpack one package
//   presets                           standard screen sizes
//   avds                              virtual devices
//   create --name N --image P [--preset ID | --size WxH --density D] [--ram MB]
//   delete <avd>
//   resize <avd> --size WxH --density D [--override]
//   start <avd> [--cold] [--wipe]     boot it (its window hidden; DevX shows the screen)
//   stop <avd>
//   screenshot <avd> [--out file.png]
//   rotate <avd> <0|90|180|270> | key <avd> <key> | tap <avd> <x> <y> | text <avd> <text>
//   controls <avd> [--pane N] | battery <avd> --level N [--charging] | gps <avd> --lat L --lng L
//
// The SDK is the one already on this Mac when there is one (ANDROID_HOME,
// ANDROID_SDK_ROOT, Android Studio's), otherwise DevX's own. Nothing is
// installed without its license having been accepted, and nothing is accepted
// except by `license <id> --accept`.
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "adapters/android/emulator/avd.hpp"
#include "adapters/android/emulator/emulator_control.hpp"
#include "adapters/android/emulator/emulator_process.hpp"
#include "adapters/android/emulator/sdk_repository.hpp"
#include "apps/cli/cli.hpp"
#include "core/observe/observation_store.hpp"

namespace mpi::cli {
namespace {

using namespace mpi::android;

std::string arg(const Invocation& inv, std::size_t i) {
  return i < inv.positional.size() ? inv.positional[i] : std::string();
}

bool parse_size(const std::string& s, int* w, int* h) {
  const auto x = s.find_first_of("xX");
  if (x == std::string::npos) return false;
  *w = std::atoi(s.substr(0, x).c_str());
  *h = std::atoi(s.substr(x + 1).c_str());
  return *w > 0 && *h > 0;
}

std::string mb(std::uint64_t n) {
  const double bytes = static_cast<double>(n);
  std::ostringstream o;
  const bool gb = n >= (1ull << 30);
  o << std::fixed << std::setprecision(gb ? 1 : 0)
    << (gb ? bytes / 1073741824.0 : bytes / 1048576.0) << (gb ? " GB" : " MB");
  return o.str();
}

// An attached control for a running AVD, or an error printed and false.
bool attach(const std::string& avd, EmulatorControl& ctl) {
  const auto r = find_running(avd);
  if (!r) {
    std::cerr << "error: '" << avd << "' is not running. `mpi emulator start " << avd << "`\n";
    return false;
  }
  std::string err;
  if (!ctl.connect(*r, &err)) {
    std::cerr << "error: cannot attach to " << avd << ": " << err << "\n";
    return false;
  }
  return true;
}

ExitCode done(bool ok, const EmulatorControl& ctl) {
  if (ok) return ExitCode::kOk;
  std::cerr << "error: " << ctl.last_error() << "\n";
  return ExitCode::kCollectionError;
}

ExitCode cmd_sdk(const Invocation& inv) {
  const auto loc = locate_sdk();
  const auto installed = installed_packages(loc.root);
  const auto running = running_emulators();
  if (inv.global.json) {
    json::Value o = json::Value::object();
    o.set("root", json::Value::string(loc.root));
    o.set("source", json::Value::string(loc.source));
    o.set("exists", json::Value::boolean(loc.exists));
    json::Value pk = json::Value::array();
    for (const auto& p : installed) {
      json::Value e = json::Value::object();
      e.set("path", json::Value::string(p.path));
      e.set("revision", json::Value::string(p.revision));
      e.set("display_name", json::Value::string(p.display_name));
      pk.push_back(std::move(e));
    }
    o.set("installed", std::move(pk));
    json::Value rn = json::Value::array();
    for (const auto& r : running) rn.push_back(r.to_json());
    o.set("running", std::move(rn));
    print_json(o);
    return ExitCode::kOk;
  }
  std::cout << "SDK " << loc.root << "  (" << loc.source << (loc.exists ? "" : ", not created yet")
            << ")\n";
  for (const auto& p : installed) {
    std::cout << "  " << std::left << std::setw(62) << p.path << p.revision << "\n";
  }
  if (installed.empty()) std::cout << "  (nothing installed: see `mpi emulator catalog`)\n";
  std::cout << "\nRUNNING\n";
  for (const auto& r : running) {
    std::cout << "  " << r.avd_id << "  " << r.serial << "  grpc:" << r.grpc_port
              << (r.attachable() ? "" : "  (not attachable: started with JWT auth)") << "\n";
  }
  if (running.empty()) std::cout << "  (none)\n";
  return ExitCode::kOk;
}

ExitCode cmd_catalog(const Invocation& inv) {
  const auto loc = locate_sdk();
  const auto cat = fetch_catalog(std::chrono::milliseconds(inv.global.timeout_ms * 4), inv.global.cancel);
  const auto installed = installed_packages(loc.root);
  if (inv.global.json) {
    print_json(cat.to_json(installed));
  } else {
    std::cout << "PACKAGE                                                        SIZE      INSTALLED\n";
    for (const auto& p : cat.packages) {
      std::string have;
      for (const auto& i : installed) {
        if (i.path == p.path) have = i.revision.empty() ? "yes" : i.revision;
      }
      std::cout << "  " << std::left << std::setw(61) << p.path << std::setw(10)
                << mb(p.archive.size) << (have.empty() ? "-" : have) << "\n";
    }
    std::cout << "\nInstall with `mpi emulator install <package>`; its license must be accepted "
                 "first (`mpi emulator license <id>`).\n";
  }
  for (const auto& e : cat.errors) warn(e);
  return cat.packages.empty() ? ExitCode::kCollectionError : ExitCode::kOk;
}

ExitCode cmd_license(const Invocation& inv) {
  const std::string id = arg(inv, 1);
  const auto loc = locate_sdk();
  const auto cat = fetch_catalog(std::chrono::milliseconds(inv.global.timeout_ms * 4), inv.global.cancel);
  const auto it = cat.licenses.find(id);
  if (id.empty() || it == cat.licenses.end()) {
    std::cerr << "error: name a license:";
    for (const auto& [k, _] : cat.licenses) std::cerr << " " << k;
    std::cerr << "\n";
    return ExitCode::kUsage;
  }
  const bool accepted = license_accepted(loc.root, id, it->second);
  if (!inv.has_flag("accept")) {
    std::cout << it->second << "\n\n"
              << (accepted ? "Accepted." : "Not accepted. Read the text above; to accept it, run "
                                           "`mpi emulator license " + id + " --accept`.")
              << "\n";
    return ExitCode::kOk;
  }
  std::string err;
  if (!accept_license(loc.root, id, it->second, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kCollectionError;
  }
  std::cout << "accepted " << id << " in " << loc.root << "/licenses\n";
  return ExitCode::kOk;
}

ExitCode cmd_install(const Invocation& inv) {
  const std::string path = arg(inv, 1);
  if (path.empty()) {
    std::cerr << "error: install needs a package path, e.g. emulator, platform-tools, or "
                 "system-images;android-35;google_apis_playstore;arm64-v8a\n";
    return ExitCode::kUsage;
  }
  const auto loc = locate_sdk();
  const auto cat = fetch_catalog(std::chrono::milliseconds(inv.global.timeout_ms * 4), inv.global.cancel);
  const SdkPackage* pkg = nullptr;
  for (const auto& p : cat.packages) {
    if (p.path == path) pkg = &p;
  }
  if (pkg == nullptr) {
    std::cerr << "error: '" << path << "' is not in the catalog for this Mac\n";
    return ExitCode::kNotFound;
  }
  const std::string text = cat.licenses.count(pkg->license_id) ? cat.licenses.at(pkg->license_id) : "";
  if (!pkg->license_id.empty() && !license_accepted(loc.root, pkg->license_id, text)) {
    std::cerr << "error: " << path << " is under the license '" << pkg->license_id
              << "', which has not been accepted. Read it with `mpi emulator license "
              << pkg->license_id << "`.\n";
    return ExitCode::kUnsupportedOperation;
  }
  std::cerr << "installing " << pkg->display_name << " (" << mb(pkg->archive.size) << ") into "
            << loc.root << "\n";
  int last_pct = -1;
  std::string last_phase;
  const auto r = install_package(*pkg, loc.root, text, [&](const InstallProgress& p) {
    if (inv.global.quiet) return;
    const int pct = p.total > 0 ? static_cast<int>(p.done * 100 / p.total) : 0;
    if (p.phase != last_phase || (p.phase == "downloading" && pct / 5 != last_pct / 5)) {
      std::cerr << "  " << p.phase;
      if (p.phase == "downloading") std::cerr << " " << pct << "%";
      std::cerr << "\n";
      last_phase = p.phase;
      last_pct = pct;
    }
  }, inv.global.cancel);
  if (!r.ok) {
    std::cerr << "error: " << r.error << "\n";
    return inv.global.cancel.cancelled() ? ExitCode::kCancelled : ExitCode::kCollectionError;
  }
  std::cout << "installed " << path << " in " << r.directory << "\n";
  return ExitCode::kOk;
}

ExitCode cmd_presets(const Invocation& inv) {
  if (inv.global.json) {
    json::Value a = json::Value::array();
    for (const auto& p : device_presets()) a.push_back(preset_json(p));
    print_json(a);
    return ExitCode::kOk;
  }
  std::cout << "ID               NAME                  PIXELS      DPI   DP          WINDOW CLASS\n";
  for (const auto& p : device_presets()) {
    std::ostringstream px, dp;
    px << p.width << "x" << p.height;
    dp << px_to_dp(p.width, p.density) << "x" << px_to_dp(p.height, p.density);
    std::cout << std::left << std::setw(17) << p.id << std::setw(22) << p.name << std::setw(12)
              << px.str() << std::setw(6) << p.density << std::setw(12) << dp.str()
              << window_size_class(p.width, p.density) << " / "
              << window_size_class(p.height, p.density) << " rotated\n";
  }
  return ExitCode::kOk;
}

ExitCode cmd_avds(const Invocation& inv) {
  const auto avds = list_avds();
  const auto running = running_emulators();
  if (inv.global.json) {
    json::Value a = json::Value::array();
    for (const auto& v : avds) {
      json::Value o = v.to_json();
      for (const auto& r : running) {
        if (r.avd_id == v.id) o.set("running", r.to_json());
      }
      a.push_back(std::move(o));
    }
    print_json(a);
    return ExitCode::kOk;
  }
  for (const auto& v : avds) {
    bool up = false;
    for (const auto& r : running) up = up || r.avd_id == v.id;
    std::cout << std::left << std::setw(28) << v.id << std::setw(12)
              << (std::to_string(v.width) + "x" + std::to_string(v.height)) << std::setw(6)
              << v.density << "API " << std::setw(5) << v.api_level << std::setw(24) << v.tag
              << (up ? "running" : "") << "\n";
  }
  if (avds.empty()) std::cout << "(no AVDs: `mpi emulator create`)\n";
  return ExitCode::kOk;
}

ExitCode cmd_create(const Invocation& inv) {
  AvdSpec spec;
  spec.display_name = inv.flag("name");
  spec.system_image = inv.flag("image");
  if (spec.display_name.empty() || spec.system_image.empty()) {
    std::cerr << "error: create needs --name and --image <system image package path>\n";
    return ExitCode::kUsage;
  }
  if (inv.has_flag("preset")) {
    const auto* p = find_preset(inv.flag("preset"));
    if (p == nullptr) {
      std::cerr << "error: no preset '" << inv.flag("preset") << "'; see `mpi emulator presets`\n";
      return ExitCode::kUsage;
    }
    spec.width = p->width;
    spec.height = p->height;
    spec.density = p->density;
    spec.device_name = p->id;
  }
  if (inv.has_flag("size") && !parse_size(inv.flag("size"), &spec.width, &spec.height)) {
    std::cerr << "error: --size is WIDTHxHEIGHT in pixels\n";
    return ExitCode::kUsage;
  }
  if (inv.has_flag("density")) spec.density = std::atoi(inv.flag("density").c_str());
  if (inv.has_flag("ram")) spec.ram_mb = std::atoi(inv.flag("ram").c_str());
  if (spec.width == 0) {
    const auto* p = find_preset("medium_phone");
    spec.width = p->width;
    spec.height = p->height;
    spec.density = p->density;
    spec.device_name = p->id;
  }
  AvdInfo out;
  std::string err;
  if (!create_avd(locate_sdk().root, spec, out, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kCollectionError;
  }
  if (inv.global.json) {
    print_json(out.to_json());
  } else {
    std::cout << "created " << out.id << " (" << out.width << "x" << out.height << " @"
              << out.density << " dpi, " << window_size_class(out.width, out.density) << ")\n";
  }
  return ExitCode::kOk;
}

ExitCode cmd_delete(const Invocation& inv) {
  const std::string id = arg(inv, 1);
  if (find_running(id)) {
    std::cerr << "error: '" << id << "' is running; stop it first\n";
    return ExitCode::kUnsupportedOperation;
  }
  std::string err;
  if (!delete_avd(id, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kNotFound;
  }
  std::cout << "deleted " << id << "\n";
  return ExitCode::kOk;
}

ExitCode cmd_resize(const Invocation& inv) {
  const std::string id = arg(inv, 1);
  int w = 0, h = 0;
  int density = inv.has_flag("density") ? std::atoi(inv.flag("density").c_str()) : 0;
  if (inv.has_flag("preset")) {
    const auto* p = find_preset(inv.flag("preset"));
    if (p == nullptr) {
      std::cerr << "error: no preset '" << inv.flag("preset") << "'\n";
      return ExitCode::kUsage;
    }
    w = p->width;
    h = p->height;
    if (density == 0) density = p->density;  // the preset's, unless one is given
  }
  if (inv.has_flag("size") && !parse_size(inv.flag("size"), &w, &h)) {
    std::cerr << "error: --size is WIDTHxHEIGHT in pixels\n";
    return ExitCode::kUsage;
  }
  std::string err;
  if (inv.has_flag("override")) {
    const auto r = find_running(id);
    if (!r) {
      std::cerr << "error: --override changes a running device; '" << id << "' is not running\n";
      return ExitCode::kUnsupportedOperation;
    }
    if (!override_screen(locate_sdk().root, r->serial, w, h, density, &err)) {
      std::cerr << "error: " << err << "\n";
      return ExitCode::kCollectionError;
    }
    std::cout << (w > 0 ? "overridden" : "reset") << " (adb shell wm): no restart needed; "
              << "the hardware size is unchanged\n";
    return ExitCode::kOk;
  }
  if (w == 0 || density == 0) {
    std::cerr << "error: resize needs --size WxH and --density D (or --preset ID)\n";
    return ExitCode::kUsage;
  }
  if (!set_avd_screen(id, w, h, density, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kCollectionError;
  }
  std::cout << "set " << id << " to " << w << "x" << h << " @" << density
            << " dpi; it takes effect at the next start, which will be a cold boot\n";
  return ExitCode::kOk;
}

ExitCode cmd_start(const Invocation& inv) {
  const std::string id = arg(inv, 1);
  LaunchOptions o;
  o.cold_boot = inv.has_flag("cold");
  o.wipe_data = inv.has_flag("wipe");
  o.cancel = inv.global.cancel;
  if (!inv.global.quiet) o.progress = [](const std::string& m) { std::cerr << m << "\n"; };
  const auto r = launch_emulator(locate_sdk().root, id, o);
  for (const auto& n : r.notes) std::cerr << "note: " << n << "\n";
  if (!r.ok) {
    std::cerr << "error: " << r.error << "\n";
    if (!r.log_path.empty()) std::cerr << "  emulator log: " << r.log_path << "\n";
    return r.started ? ExitCode::kInconclusive : ExitCode::kCollectionError;
  }
  if (inv.global.json) {
    print_json(r.emulator.to_json());
  } else {
    std::cout << "running: " << r.emulator.avd_id << " as " << r.emulator.serial << "\n";
  }
  return ExitCode::kOk;
}

ExitCode cmd_stop(const Invocation& inv) {
  const auto r = find_running(arg(inv, 1));
  if (!r) {
    std::cerr << "error: '" << arg(inv, 1) << "' is not running\n";
    return ExitCode::kNotFound;
  }
  std::string err;
  if (!stop_emulator(locate_sdk().root, *r, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kCollectionError;
  }
  std::cout << "stopped " << r->avd_id << "\n";
  return ExitCode::kOk;
}

ExitCode cmd_screenshot(const Invocation& inv) {
  const std::string id = arg(inv, 1);
  EmulatorControl ctl;
  if (!attach(id, ctl)) return ExitCode::kNotFound;
  std::string png, err;
  if (!ctl.screenshot_png(&png, 0, 0, &err)) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kCollectionError;
  }
  const std::string out = inv.flag("out", id + ".png");
  std::ofstream(out, std::ios::binary) << png;
  // Kept for AI tools too: the image path and the device it shows.
  json::Value doc = json::Value::object();
  doc.set("avd_id", json::Value::string(id));
  doc.set("serial", json::Value::string(ctl.emulator().serial));
  doc.set("png_path", json::Value::string(out));
  doc.set("bytes", json::Value::integer(static_cast<std::int64_t>(png.size())));
  json::Value st;
  if (ctl.status(&st, nullptr)) doc.set("status", st);
  if (!inv.has_flag("no-save")) {
    observe::save_observation(inv.global.sessions_dir, "emulator_screenshot", id,
                              ctl.emulator().serial, json::Value::object(), doc);
  }
  std::cout << "saved " << out << " (" << png.size() << " bytes)\n";
  return ExitCode::kOk;
}

}  // namespace

ExitCode cmd_emulator(const Invocation& inv) {
  const std::string sub = arg(inv, 0);
  if (sub == "sdk" || sub.empty()) return cmd_sdk(inv);
  if (sub == "catalog") return cmd_catalog(inv);
  if (sub == "license") return cmd_license(inv);
  if (sub == "install") return cmd_install(inv);
  if (sub == "presets") return cmd_presets(inv);
  if (sub == "avds") return cmd_avds(inv);
  if (sub == "create") return cmd_create(inv);
  if (sub == "delete") return cmd_delete(inv);
  if (sub == "resize") return cmd_resize(inv);
  if (sub == "start") return cmd_start(inv);
  if (sub == "stop") return cmd_stop(inv);
  if (sub == "screenshot") return cmd_screenshot(inv);

  // The rest drive a running device.
  EmulatorControl ctl;
  if (sub == "rotate" || sub == "key" || sub == "tap" || sub == "text" || sub == "controls" ||
      sub == "battery" || sub == "gps") {
    if (!attach(arg(inv, 1), ctl)) return ExitCode::kNotFound;
  }
  if (sub == "rotate") return done(ctl.rotate(std::atoi(arg(inv, 2).c_str())), ctl);
  if (sub == "key") return done(ctl.key(arg(inv, 2)), ctl);
  if (sub == "text") return done(ctl.text(arg(inv, 2)), ctl);
  if (sub == "tap") {
    const int x = std::atoi(arg(inv, 2).c_str()), y = std::atoi(arg(inv, 3).c_str());
    return done(ctl.touch(x, y, 1024) && ctl.touch(x, y, 0), ctl);
  }
  if (sub == "controls") {
    return done(ctl.show_extended_controls(std::atoi(inv.flag("pane", "0").c_str())), ctl);
  }
  if (sub == "battery") {
    return done(ctl.set_battery(std::atoi(inv.flag("level", "100").c_str()),
                                inv.has_flag("charging")),
                ctl);
  }
  if (sub == "gps") {
    return done(ctl.set_gps(std::atof(inv.flag("lat").c_str()), std::atof(inv.flag("lng").c_str())),
                ctl);
  }
  std::cerr << "error: unknown emulator command '" << sub << "'. See `mpi help`.\n";
  return ExitCode::kUsage;
}

}  // namespace mpi::cli
