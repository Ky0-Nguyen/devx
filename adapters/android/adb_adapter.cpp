#include "adapters/android/adb_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

#include "core/util/process.hpp"
#include "core/util/time.hpp"

namespace mpi::android {
namespace {

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

std::vector<std::string> split_ws(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool all_digits(const std::string& s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// "u0_a123" -> user 0, app id 123 -> uid 10123. Returns nullopt for a form we
// do not recognize, rather than inventing a uid.
struct AndroidUser {
  int user_id = 0;
  std::int32_t uid = 0;
};
std::optional<AndroidUser> parse_android_user(const std::string& user) {
  if (all_digits(user)) {
    const auto raw = static_cast<std::int32_t>(std::atoll(user.c_str()));
    AndroidUser a;
    a.uid = raw;
    a.user_id = raw / 100000;
    return a;
  }
  if (user.size() < 4 || user[0] != 'u') return std::nullopt;
  const std::size_t underscore = user.find('_');
  if (underscore == std::string::npos) return std::nullopt;
  const std::string user_part = user.substr(1, underscore - 1);
  std::string app_part = user.substr(underscore + 1);
  if (!all_digits(user_part)) return std::nullopt;
  // Forms: a<appid> (regular app), i<appid> (isolated), s<n> (shared/system).
  if (app_part.empty()) return std::nullopt;
  const char kind = app_part[0];
  app_part = app_part.substr(1);
  if (!all_digits(app_part)) return std::nullopt;
  AndroidUser a;
  a.user_id = std::atoi(user_part.c_str());
  const std::int32_t app_id = static_cast<std::int32_t>(std::atoll(app_part.c_str()));
  // Base offsets from Android's Process/UserHandle constants.
  if (kind == 'a') {
    a.uid = a.user_id * 100000 + 10000 + app_id;
  } else if (kind == 'i') {
    a.uid = a.user_id * 100000 + 99000 + app_id;
  } else {
    a.uid = a.user_id * 100000 + app_id;
  }
  return a;
}

// An isolated process runs under a u<N>_i<M> identity and cannot be attributed
// to a package by uid alone (spec B07).
bool is_isolated_user(const std::string& user) {
  const std::size_t underscore = user.find('_');
  return underscore != std::string::npos && underscore + 1 < user.size() &&
         user[underscore + 1] == 'i';
}

model::TestedState verified_state_for(const model::DeviceRef& d) {
  return d.form == model::DeviceForm::kPhysical
             ? model::TestedState::kVerifiedOnPhysicalDevice
             : model::TestedState::kVerifiedOnSimulatorOrEmulator;
}

std::string iso_now() { return time_util::now_iso8601_utc(); }

model::Capability make_cap(const char* id, const char* human,
                           model::CapabilityStatus status, const char* provider) {
  model::Capability c;
  c.id = id;
  c.human_name = human;
  c.status = status;
  c.provider = provider;
  c.observed_at = iso_now();
  return c;
}

}  // namespace

std::vector<PsRow> parse_ps_output(const std::string& text,
                                   std::vector<std::string>& warnings) {
  std::vector<PsRow> rows;
  const auto lines = split_lines(text);
  if (lines.empty()) return rows;

  // Read the header so column order changes across Android releases do not
  // silently shift the fields we read.
  std::map<std::string, std::size_t> col;
  std::size_t header_index = std::string::npos;
  for (std::size_t i = 0; i < lines.size() && i < 4; ++i) {
    const auto toks = split_ws(lines[i]);
    if (toks.empty()) continue;
    bool looks_like_header = false;
    for (const auto& t : toks) {
      std::string up;
      for (const char c : t) up.push_back(static_cast<char>(std::toupper(c)));
      if (up == "PID" || up == "NAME" || up == "CMD" || up == "USER") {
        looks_like_header = true;
      }
    }
    if (!looks_like_header) continue;
    for (std::size_t j = 0; j < toks.size(); ++j) {
      std::string up;
      for (const char c : toks[j]) up.push_back(static_cast<char>(std::toupper(c)));
      col[up] = j;
    }
    header_index = i;
    break;
  }
  if (header_index == std::string::npos || col.find("PID") == col.end()) {
    warnings.push_back(
        "could not identify the `ps` header; process enumeration was not "
        "attempted rather than parsed by guesswork");
    return rows;
  }
  const std::size_t name_col = col.count("NAME")   ? col.at("NAME")
                               : col.count("CMD")  ? col.at("CMD")
                                                   : std::string::npos;
  if (name_col == std::string::npos) {
    warnings.push_back("`ps` output has no NAME or CMD column");
    return rows;
  }

  for (std::size_t i = header_index + 1; i < lines.size(); ++i) {
    const auto toks = split_ws(lines[i]);
    if (toks.size() <= std::max(col.at("PID"), name_col)) continue;
    const std::string& pid_s = toks[col.at("PID")];
    if (!all_digits(pid_s)) continue;
    PsRow r;
    r.pid = static_cast<std::int32_t>(std::atoll(pid_s.c_str()));
    r.name = toks[name_col];
    if (col.count("PPID") && toks.size() > col.at("PPID")) {
      const std::string& p = toks[col.at("PPID")];
      if (all_digits(p)) r.ppid = static_cast<std::int32_t>(std::atoll(p.c_str()));
    }
    if (col.count("USER") && toks.size() > col.at("USER")) {
      r.user = toks[col.at("USER")];
      if (auto au = parse_android_user(r.user)) {
        r.uid = au->uid;
        r.android_user_id = au->user_id;
      }
    }
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<model::DeviceRef> parse_adb_devices(const std::string& text,
                                                const std::string& adb_version) {
  std::vector<model::DeviceRef> out;
  for (const auto& line : split_lines(text)) {
    const std::string t = trim(line);
    if (t.empty()) continue;
    if (t.rfind("List of devices", 0) == 0) continue;
    if (t.rfind("* daemon", 0) == 0) continue;
    const auto toks = split_ws(t);
    if (toks.size() < 2) continue;

    model::DeviceRef d;
    d.platform = model::Platform::kAndroid;
    d.device_id = toks[0];
    d.provider = "adb";
    d.observed_at = iso_now();
    d.os_version = "";  // filled in later by a per-device getprop call

    const std::string& state = toks[1];
    if (state == "device") {
      d.trust = model::TrustState::kAuthorized;
    } else if (state == "unauthorized") {
      d.trust = model::TrustState::kUnauthorized;
    } else if (state == "offline") {
      d.trust = model::TrustState::kOffline;
    } else {
      // "no permissions", "recovery", "sideload", "bootloader", ...
      d.trust = model::TrustState::kUnknown;
    }

    // An emulator is never equated with a physical device (spec 4.1, J18).
    if (d.device_id.rfind("emulator-", 0) == 0) {
      d.form = model::DeviceForm::kEmulator;
    } else {
      d.form = model::DeviceForm::kPhysical;
    }
    // A serial of the form host:port means the transport is wireless/TCP.
    d.connection = d.device_id.find(':') != std::string::npos
                       ? model::ConnectionType::kWireless
                       : model::ConnectionType::kUsb;

    // `-l` appends "key:value" pairs.
    for (std::size_t i = 2; i < toks.size(); ++i) {
      const std::size_t colon = toks[i].find(':');
      if (colon == std::string::npos) continue;
      const std::string key = toks[i].substr(0, colon);
      const std::string value = toks[i].substr(colon + 1);
      if (key == "model") {
        d.model = value;
      } else if (key == "device") {
        if (d.model.empty()) d.model = value;
      } else if (key == "product") {
        d.display_name = value;
      }
    }
    if (d.display_name.empty()) {
      d.display_name = d.model.empty() ? d.device_id : d.model;
    }
    if (!adb_version.empty()) d.provider = "adb " + adb_version;
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<PackageRow> parse_pm_list_packages(const std::string& text) {
  std::vector<PackageRow> out;
  for (const auto& line : split_lines(text)) {
    const std::string t = trim(line);
    if (t.rfind("package:", 0) != 0) continue;
    std::string rest = t.substr(8);
    PackageRow r;
    // With -U the line is "package:NAME uid:UID".
    const std::size_t sp = rest.find(" uid:");
    if (sp != std::string::npos) {
      r.package = rest.substr(0, sp);
      const std::string uid_s = trim(rest.substr(sp + 5));
      if (all_digits(uid_s)) {
        r.uid = static_cast<std::int32_t>(std::atoll(uid_s.c_str()));
      }
    } else {
      // Without -U, or with an APK path prefix, take the last '=' segment.
      const std::size_t eq = rest.rfind('=');
      r.package = eq == std::string::npos ? rest : rest.substr(eq + 1);
    }
    r.package = trim(r.package);
    if (!r.package.empty()) out.push_back(std::move(r));
  }
  return out;
}

std::optional<std::string> parse_proc_stat_starttime(const std::string& stat_line) {
  // /proc/pid/stat: field 2 is the comm name in parentheses and may itself
  // contain spaces, so parsing must start after the last ')'.
  const std::size_t rparen = stat_line.rfind(')');
  if (rparen == std::string::npos) return std::nullopt;
  const auto fields = split_ws(stat_line.substr(rparen + 1));
  // After the ')' the next field is field 3 (state); starttime is field 22,
  // i.e. index 19 in this slice.
  if (fields.size() < 20) return std::nullopt;
  const std::string& starttime = fields[19];
  if (!all_digits(starttime)) return std::nullopt;
  return starttime;
}

PackageFlags parse_dumpsys_package_flags(const std::string& text) {
  PackageFlags f;
  for (const auto& line : split_lines(text)) {
    const std::string t = trim(line);
    if (t.rfind("flags=", 0) == 0 || t.find(" flags=[") != std::string::npos) {
      // The flag list contains DEBUGGABLE when android:debuggable is set.
      f.debuggable = t.find("DEBUGGABLE") != std::string::npos;
    }
    if (t.rfind("versionName=", 0) == 0) {
      f.version_name = t.substr(12);
    }
    if (t.rfind("versionCode=", 0) == 0) {
      const auto toks = split_ws(t.substr(12));
      if (!toks.empty() && all_digits(toks[0])) {
        f.version_code = std::atoll(toks[0].c_str());
      }
    }
  }
  return f;
}

AdbAdapter::AdbAdapter(std::string adb_path) : adb_path_(std::move(adb_path)) {}

std::vector<std::string> AdbAdapter::shell_argv(
    const std::string& serial, const std::vector<std::string>& args) const {
  std::vector<std::string> argv{adb_path_};
  if (!serial.empty()) {
    argv.push_back("-s");
    argv.push_back(serial);
  }
  argv.push_back("shell");
  for (const auto& a : args) argv.push_back(a);
  return argv;
}

std::string AdbAdapter::adb_version(const discovery::ProviderOptions& opts) const {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;
  const auto r = proc::run({adb_path_, "--version"}, po);
  if (!r.ok()) return {};
  // First line: "Android Debug Bridge version 1.0.41".
  const auto lines = split_lines(r.out);
  if (lines.empty()) return {};
  const auto toks = split_ws(lines[0]);
  return toks.empty() ? std::string() : toks.back();
}

void AdbAdapter::probe(model::CapabilityMatrix& out,
                       const discovery::ProviderOptions& opts) const {
  const auto resolved = proc::which(adb_path_);
  if (!resolved.has_value()) {
    auto c = make_cap("android.toolchain.adb", "Android Platform Tools (adb)",
                      model::CapabilityStatus::kUnsupported, "adb");
    c.evidence = "`" + adb_path_ + "` was not found on PATH";
    c.prerequisites.push_back("Android SDK Platform Tools installed and on PATH");
    c.recovery_action =
        "install Android SDK Platform Tools and ensure `adb` is on PATH, or "
        "point the tool at an explicit adb path";
    c.tested = model::TestedState::kProbedOnly;
    out.upsert(std::move(c));
    // Without adb, every downstream Android capability is unsupported for a
    // stated reason rather than unknown.
    for (const auto* id : {"android.discovery.devices", "android.discovery.installed_apps",
                           "android.discovery.running_processes",
                           "android.discovery.process_mapping"}) {
      auto d = make_cap(id, id, model::CapabilityStatus::kUnsupported, "adb");
      d.evidence = "depends on adb, which is absent";
      d.recovery_action = "install Android SDK Platform Tools";
      d.tested = model::TestedState::kProbedOnly;
      out.upsert(std::move(d));
    }
    return;
  }

  const std::string version = adb_version(opts);
  {
    auto c = make_cap("android.toolchain.adb", "Android Platform Tools (adb)",
                      model::CapabilityStatus::kAvailable, "adb");
    c.provider_version = version;
    c.evidence = "`" + *resolved + " --version` reported " +
                 (version.empty() ? "an unparsed version" : version);
    c.tested = model::TestedState::kProbedOnly;
    out.upsert(std::move(c));
  }

  std::vector<std::string> errors;
  const auto devices = list_devices(opts, errors);
  {
    auto c = make_cap("android.discovery.devices", "Android device discovery",
                      model::CapabilityStatus::kAvailable, "adb");
    c.provider_version = version;
    c.evidence = "`adb devices -l` returned " + std::to_string(devices.size()) +
                 " device line(s)";
    c.scope =
        "devices visible to this host's adb server; a device claimed by "
        "another adb server or an IDE may not appear";
    bool any_physical = false;
    for (const auto& d : devices) {
      if (d.form == model::DeviceForm::kPhysical) any_physical = true;
    }
    c.tested = devices.empty()
                   ? model::TestedState::kProbedOnly
                   : (any_physical
                          ? model::TestedState::kVerifiedOnPhysicalDevice
                          : model::TestedState::kVerifiedOnSimulatorOrEmulator);
    if (!devices.empty() && !any_physical) {
      c.limitations.push_back(
          "every discovered Android device is an emulator; discovery is not "
          "verified against physical hardware");
    }
    if (devices.empty()) {
      c.limitations.push_back(
          "no device was connected at probe time, so device discovery is "
          "probed but not verified against hardware");
    }
    for (const auto& e : errors) c.limitations.push_back(e);
    out.upsert(std::move(c));
  }

  // Whether we can enumerate installed apps and running processes can only be
  // established against a real, authorized device.
  const model::DeviceRef* usable = nullptr;
  for (const auto& d : devices) {
    if (d.usable_for_capture()) {
      usable = &d;
      break;
    }
  }
  if (!usable) {
    for (const auto* id : {"android.discovery.installed_apps",
                           "android.discovery.running_processes",
                           "android.discovery.process_mapping",
                           "android.capture.profileable"}) {
      auto c = make_cap(id, id, model::CapabilityStatus::kUnknown, "adb");
      c.provider_version = version;
      c.evidence =
          devices.empty()
              ? "no Android device was connected, so this could not be probed"
              : "no connected Android device was in the `device` (authorized) "
                "state";
      c.prerequisites.push_back("an authorized Android device connected over USB or TCP");
      c.recovery_action =
          devices.empty()
              ? "connect an Android device with USB debugging enabled"
              : "accept the USB debugging prompt on the device so it reports "
                "state `device`";
      c.tested = model::TestedState::kNotTested;
      out.upsert(std::move(c));
    }
    return;
  }

  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;

  {
    const auto r = proc::run(shell_argv(usable->device_id, {"pm", "list", "packages", "-U"}), po);
    auto c = make_cap("android.discovery.installed_apps",
                      "Installed package enumeration", model::CapabilityStatus::kUnknown,
                      "adb shell pm");
    c.provider_version = version;
    if (r.ok()) {
      const auto pkgs = parse_pm_list_packages(r.out);
      c.status = pkgs.empty() ? model::CapabilityStatus::kLimited
                              : model::CapabilityStatus::kAvailable;
      c.evidence = "`pm list packages -U` returned " + std::to_string(pkgs.size()) +
                   " package(s)";
      c.scope = "packages visible to the shell user for the queried Android user";
      c.limitations.push_back(
          "installed does not mean running, and does not mean profileable");
      c.tested = verified_state_for(*usable);
    } else {
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "`pm list packages -U` failed: " +
                   (r.spawned ? ("exit " + std::to_string(r.exit_code) + ": " + trim(r.err))
                              : r.spawn_error);
      c.recovery_action = "confirm the device is authorized and the shell user can run `pm`";
      c.tested = model::TestedState::kProbedOnly;
    }
    out.upsert(std::move(c));
  }

  {
    const auto r = proc::run(shell_argv(usable->device_id, {"ps", "-A", "-o", "PID,PPID,USER,NAME"}), po);
    auto c = make_cap("android.discovery.running_processes",
                      "Running process enumeration", model::CapabilityStatus::kUnknown,
                      "adb shell ps");
    c.provider_version = version;
    std::vector<std::string> warnings;
    if (r.ok()) {
      const auto rows = parse_ps_output(r.out, warnings);
      c.status = rows.empty() ? model::CapabilityStatus::kLimited
                              : model::CapabilityStatus::kAvailable;
      c.evidence = "`ps -A -o PID,PPID,USER,NAME` returned " +
                   std::to_string(rows.size()) + " parsed row(s)";
      c.scope =
          "processes visible to the shell user; shell has broader visibility "
          "than an ordinary app SDK would";
      c.tested = verified_state_for(*usable);
    } else {
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "`ps` failed: " + (r.spawned ? trim(r.err) : r.spawn_error);
      c.tested = model::TestedState::kProbedOnly;
    }
    for (const auto& w : warnings) c.limitations.push_back(w);
    out.upsert(std::move(c));
  }

  {
    // Reading /proc/<pid>/stat is what rules out PID reuse. Probe it on our
    // own shell process, which always exists.
    const auto r = proc::run(shell_argv(usable->device_id, {"cat", "/proc/self/stat"}), po);
    auto c = make_cap("android.discovery.process_mapping",
                      "Process start-time / identity resolution",
                      model::CapabilityStatus::kUnknown, "adb shell /proc");
    c.provider_version = version;
    if (r.ok() && parse_proc_stat_starttime(r.out).has_value()) {
      c.status = model::CapabilityStatus::kAvailable;
      c.evidence = "/proc/self/stat parsed; field 22 (starttime) is readable";
      c.scope =
          "start time lets a process instance be distinguished across PID "
          "reuse and device reboot";
      c.tested = verified_state_for(*usable);
    } else {
      c.status = model::CapabilityStatus::kLimited;
      c.evidence = "could not read or parse /proc/self/stat";
      c.limitations.push_back(
          "without a process start time, PID reuse cannot be ruled out and "
          "ownership is downgraded accordingly");
      c.tested = model::TestedState::kProbedOnly;
    }
    out.upsert(std::move(c));
  }

  {
    // Deep profiling depends on the build, not on adb. Say so rather than
    // implying adb grants it.
    auto c = make_cap("android.capture.profileable",
                      "Permission to profile a selected app",
                      model::CapabilityStatus::kUnknown, "adb");
    c.provider_version = version;
    c.evidence =
        "not determinable at the device level: it depends on the selected "
        "package's manifest (<profileable> / android:debuggable) and the "
        "device build type";
    c.prerequisites.push_back(
        "the target APK declares <profileable android:shell=\"true\"/> or is "
        "debuggable, or the device runs a userdebug/eng build");
    c.scope = "must be re-probed per selected package during preflight";
    c.recovery_action =
        "add <profileable android:shell=\"true\"/> to the target's manifest "
        "and reinstall";
    c.tested = model::TestedState::kNotTested;
    out.upsert(std::move(c));
  }
}

std::vector<model::DeviceRef> AdbAdapter::list_devices(
    const discovery::ProviderOptions& opts, std::vector<std::string>& errors) const {
  if (!proc::which(adb_path_).has_value()) {
    errors.push_back("adb not found on PATH; Android devices cannot be enumerated");
    return {};
  }
  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;
  const auto r = proc::run({adb_path_, "devices", "-l"}, po);
  if (!r.spawned) {
    errors.push_back("could not start adb: " + r.spawn_error);
    return {};
  }
  if (r.timed_out) {
    errors.push_back("`adb devices -l` timed out after " +
                     std::to_string(opts.command_timeout_ms) + " ms");
    return {};
  }
  if (r.exit_code != 0) {
    errors.push_back("`adb devices -l` exited " + std::to_string(r.exit_code) +
                     ": " + trim(r.err));
    return {};
  }

  auto devices = parse_adb_devices(r.out, adb_version(opts));
  // Fill in OS version and boot identity per authorized device.
  for (auto& d : devices) {
    if (!d.usable_for_capture()) continue;
    const auto rel = proc::run(shell_argv(d.device_id, {"getprop", "ro.build.version.release"}), po);
    if (rel.ok()) d.os_version = trim(rel.out);
    if (d.model.empty()) {
      const auto mdl = proc::run(shell_argv(d.device_id, {"getprop", "ro.product.model"}), po);
      if (mdl.ok()) d.model = trim(mdl.out);
    }
    // boot_id changes on reboot, which invalidates every prior process
    // identity for this device (spec B04).
    const auto boot = proc::run(shell_argv(d.device_id, {"cat", "/proc/sys/kernel/random/boot_id"}), po);
    if (boot.ok()) d.boot_id = trim(boot.out);
    // A userdebug/eng build changes what is profileable.
    const auto type = proc::run(shell_argv(d.device_id, {"getprop", "ro.build.type"}), po);
    if (type.ok() && !trim(type.out).empty()) {
      d.provider += " (build.type=" + trim(type.out) + ")";
    }
  }
  return devices;
}

std::vector<model::AppEntry> AdbAdapter::list_apps(
    const model::DeviceRef& device, const discovery::ProviderOptions& opts,
    std::vector<std::string>& errors, bool& enumeration_failed) const {
  enumeration_failed = false;
  std::vector<model::AppEntry> out;
  if (!device.usable_for_capture()) {
    errors.push_back("device " + device.device_id + " is in state " +
                     model::to_string(device.trust) +
                     "; apps cannot be enumerated");
    enumeration_failed = true;
    return out;
  }

  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;

  // Input 1: installed packages with their uids, per Android user.
  std::vector<int> users{0};
  if (const auto ul = proc::run(shell_argv(device.device_id, {"pm", "list", "users"}), po);
      ul.ok()) {
    users.clear();
    for (const auto& line : split_lines(ul.out)) {
      // "	UserInfo{0:Owner:c13} running"
      const std::size_t brace = line.find('{');
      if (brace == std::string::npos) continue;
      const std::size_t colon = line.find(':', brace);
      if (colon == std::string::npos) continue;
      const std::string id = trim(line.substr(brace + 1, colon - brace - 1));
      if (all_digits(id)) users.push_back(std::atoi(id.c_str()));
    }
    if (users.empty()) users.push_back(0);
  } else {
    errors.push_back(
        "could not list Android users; assuming user 0 only, so a work "
        "profile's apps may be missing from this listing");
  }

  bool any_package_listing_ok = false;
  // package -> (user -> uid)
  std::map<std::string, std::map<int, std::optional<std::int32_t>>> installed;
  for (const int user : users) {
    if (opts.cancel.cancelled()) break;
    const auto r = proc::run(
        shell_argv(device.device_id,
                   {"pm", "list", "packages", "-U", "--user", std::to_string(user)}),
        po);
    if (!r.ok()) {
      errors.push_back("`pm list packages --user " + std::to_string(user) +
                       "` failed: " + (r.spawned ? trim(r.err) : r.spawn_error));
      continue;
    }
    any_package_listing_ok = true;
    for (const auto& p : parse_pm_list_packages(r.out)) {
      installed[p.package][user] = p.uid;
    }
  }

  // Input 2: live processes. Reconciled against input 1, never trusted alone.
  std::vector<PsRow> ps_rows;
  bool ps_ok = false;
  {
    std::vector<std::string> warnings;
    const auto r = proc::run(
        shell_argv(device.device_id, {"ps", "-A", "-o", "PID,PPID,USER,NAME"}), po);
    if (r.ok()) {
      ps_rows = parse_ps_output(r.out, warnings);
      ps_ok = !ps_rows.empty();
    } else {
      errors.push_back("`ps -A` failed: " +
                       (r.spawned ? trim(r.err) : r.spawn_error));
    }
    for (const auto& w : warnings) errors.push_back(w);
  }

  if (!any_package_listing_ok && !ps_ok) {
    // Spec A12: enumeration failure is a different answer from an empty list.
    enumeration_failed = true;
    errors.push_back(
        "neither package enumeration nor process enumeration succeeded; the "
        "app list is unavailable, which is not the same as the device having "
        "no apps");
    return out;
  }

  const std::string observed = iso_now();

  for (const auto& pkg : installed) {
    for (const auto& user_uid : pkg.second) {
      model::AppEntry e;
      e.key.platform = model::Platform::kAndroid;
      e.key.device_id = device.device_id;
      e.key.app_identifier = pkg.first;
      e.key.identifier_kind = model::IdentifierKind::kPackageName;
      if (users.size() > 1 || user_uid.first != 0) {
        e.key.android_user_id = user_uid.first;
      }
      e.display_name = pkg.first;  // a friendly label needs a separate query
      e.installed_known = true;
      e.installed = true;
      e.provider = "adb shell pm";
      e.observed_at = observed;
      e.visibility_scope = any_package_listing_ok
                               ? model::DiscoveryScope::kCompleteForProvider
                               : model::DiscoveryScope::kPartial;

      if (!ps_ok) {
        // Spec A11 / A18: unknown must not be rendered as not_running.
        e.runtime_state = model::RuntimeState::kUnknown;
        e.notes.push_back(
            "process enumeration was unavailable, so the runtime state is "
            "unknown rather than not running");
      } else {
        std::vector<model::ProcessInstance> procs;
        for (const auto& row : ps_rows) {
          // A process belongs to this package when the process name is the
          // package name or a package-declared sub-process (`pkg:name`).
          const bool name_is_package = row.name == pkg.first;
          const bool name_is_subprocess =
              row.name.size() > pkg.first.size() + 1 &&
              row.name.rfind(pkg.first + ":", 0) == 0;
          if (!name_is_package && !name_is_subprocess) continue;
          if (row.android_user_id.has_value() &&
              *row.android_user_id != user_uid.first) {
            continue;  // a different Android user's instance
          }

          model::ProcessInstance p;
          p.app = e.key;
          p.pid = row.pid;
          p.process_name = row.name;
          p.uid = row.uid;
          p.is_primary = name_is_package;
          p.boot_id = device.boot_id;

          // Ownership: the uid must match the package's uid *and* the process
          // name must be package-owned. Either alone is not enough.
          const bool uid_matches = user_uid.second.has_value() &&
                                   row.uid.has_value() &&
                                   *user_uid.second == *row.uid;
          if (is_isolated_user(row.user)) {
            // Spec B07: an isolated process cannot be attributed by uid.
            p.ownership = model::OwnershipEvidence::kAmbiguous;
            p.ownership_note =
                "isolated process (" + row.user +
                "): its uid is not the package uid, so ownership cannot be "
                "established from process metadata alone";
          } else if (uid_matches) {
            p.ownership = model::OwnershipEvidence::kUidAndProcessName;
            p.ownership_note =
                "uid " + std::to_string(*row.uid) +
                " matches the installed package uid and the process name is "
                "package-owned";
          } else if (!user_uid.second.has_value()) {
            p.ownership = model::OwnershipEvidence::kAmbiguous;
            p.ownership_note =
                "the package's uid was not reported, so a name match alone "
                "does not establish ownership";
          } else {
            // Same name, different uid: a shared-uid sibling or a spoofed
            // name. Spec B06 says a shared uid is not sufficient evidence.
            p.ownership = model::OwnershipEvidence::kAmbiguous;
            p.ownership_note =
                "process name matches but uid " +
                (row.uid.has_value() ? std::to_string(*row.uid)
                                     : std::string("unknown")) +
                " differs from the package uid " +
                std::to_string(*user_uid.second) +
                "; possibly a shared-uid sibling";
          }

          // Start time distinguishes this instance from a later PID reuse.
          const auto stat = proc::run(
              shell_argv(device.device_id,
                         {"cat", "/proc/" + std::to_string(row.pid) + "/stat"}),
              po);
          if (stat.ok()) {
            if (auto st = parse_proc_stat_starttime(stat.out)) {
              p.process_start_time = *st;
            }
          }
          if (p.process_start_time.empty()) {
            p.ownership_note +=
                "; no process start time was readable, so PID reuse cannot be "
                "ruled out";
            if (p.ownership == model::OwnershipEvidence::kUidAndProcessName) {
              p.ownership = model::OwnershipEvidence::kAmbiguous;
            }
          }
          procs.push_back(std::move(p));
        }
        e.processes = std::move(procs);
        e.runtime_state = e.processes.empty() ? model::RuntimeState::kNotRunning
                                              : model::RuntimeState::kRunning;
        if (!e.processes.empty()) {
          // Spec 3.2: running does not mean foreground, and this provider does
          // not observe foreground state.
          e.notes.push_back(
              "running, but foreground/background state is not observed by "
              "this provider");
        }
      }

      // Profiling availability is a separate capability from visibility.
      e.profiling = model::ProfilingAvailability::kUnknown;
      e.profiling_reason =
          "requires the package's manifest flags and the device build type; "
          "resolved during preflight for the selected app";
      e.profiling_recovery_action =
          "declare <profileable android:shell=\"true\"/> in the target "
          "manifest, or use a debuggable build, or a userdebug device";
      out.push_back(std::move(e));
    }
  }

  // Processes whose name matches no installed package: listed as unassigned
  // rather than dropped or attributed by guess.
  if (ps_ok) {
    for (const auto& row : ps_rows) {
      const std::size_t colon = row.name.find(':');
      const std::string base = colon == std::string::npos ? row.name
                                                          : row.name.substr(0, colon);
      if (installed.count(base)) continue;
      // Only report things that look like an app package (contain a dot and no
      // leading slash), so the whole system process table is not dumped.
      if (base.find('.') == std::string::npos || base.front() == '/') continue;
      if (base.rfind("com.android.", 0) == 0 || base.rfind("android.", 0) == 0) {
        continue;  // framework processes, not app targets
      }
      model::AppEntry e;
      e.key.platform = model::Platform::kAndroid;
      e.key.device_id = device.device_id;
      e.key.app_identifier = base;
      e.key.identifier_kind = model::IdentifierKind::kPackageName;
      e.display_name = base;
      e.installed_known = any_package_listing_ok;
      e.installed = false;
      e.runtime_state = model::RuntimeState::kRunning;
      e.visibility_scope = model::DiscoveryScope::kPartial;
      e.provider = "adb shell ps";
      e.observed_at = observed;
      e.profiling = model::ProfilingAvailability::kUnknown;
      e.profiling_reason =
          "a running process with this name exists but it matches no package "
          "visible to the shell user";
      e.notes.push_back(
          "seen only in the process table, not in the package listing; "
          "ownership is unresolved");
      model::ProcessInstance p;
      p.app = e.key;
      p.pid = row.pid;
      p.process_name = row.name;
      p.uid = row.uid;
      p.boot_id = device.boot_id;
      p.ownership = model::OwnershipEvidence::kAmbiguous;
      p.ownership_note =
          "no installed package matched this process name for the shell user";
      e.processes.push_back(std::move(p));
      out.push_back(std::move(e));
    }
  }

  std::sort(out.begin(), out.end(),
            [](const model::AppEntry& a, const model::AppEntry& b) {
              return a.key.canonical() < b.key.canonical();
            });
  return out;
}

std::vector<model::ProcessInstance> AdbAdapter::resolve_processes(
    const model::DeviceRef& device, const model::ApplicationKey& app,
    const discovery::ProviderOptions& opts, std::vector<std::string>& errors) const {
  // Refuse an identifier that could be reinterpreted as a flag by the invoked
  // tool. The argv is never a shell string, but an identifier starting with
  // '-' would still be read as an option by `pm`/`ps`.
  if (!proc::is_safe_argument(app.app_identifier, /*reject_option_like=*/true)) {
    errors.push_back("refusing to query the identifier '" + app.app_identifier +
                     "': it would be interpreted as a command-line option");
    return {};
  }
  bool failed = false;
  std::vector<model::AppEntry> apps = list_apps(device, opts, errors, failed);
  for (const auto& e : apps) {
    if (e.key.app_identifier != app.app_identifier) continue;
    if (app.android_user_id.has_value() &&
        e.key.android_user_id != app.android_user_id) {
      continue;
    }
    return e.processes;
  }
  if (!failed) {
    errors.push_back("no process instance of '" + app.app_identifier +
                     "' was observed on " + device.device_id +
                     " at " + iso_now());
  }
  return {};
}

}  // namespace mpi::android
