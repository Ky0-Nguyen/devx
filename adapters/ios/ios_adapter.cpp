#include "adapters/ios/ios_adapter.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/util/process.hpp"
#include "core/util/time.hpp"

namespace mpi::ios {
namespace {

std::string iso_now() { return time_util::now_iso8601_utc(); }

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (!v) return {};
  if (v->is_string()) return v->as_string();
  if (v->is_number()) return std::to_string(v->as_int());
  return {};
}

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

// A unique temp path for devicectl's --json-output. Created with mkstemp so
// no other process can win a race on the name.
class TempJsonFile {
 public:
  TempJsonFile() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tpl = (tmpdir && *tmpdir ? std::string(tmpdir) : std::string("/tmp"));
    if (!tpl.empty() && tpl.back() != '/') tpl.push_back('/');
    tpl += "mpi-devicectl-XXXXXX";
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd >= 0) {
      ::close(fd);
      path_ = buf.data();
    }
  }
  ~TempJsonFile() {
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  TempJsonFile(const TempJsonFile&) = delete;
  TempJsonFile& operator=(const TempJsonFile&) = delete;
  const std::string& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

 private:
  std::string path_;
};

}  // namespace

std::vector<model::DeviceRef> parse_devicectl_devices(
    const json::Value& root, const std::string& provider_version) {
  std::vector<model::DeviceRef> out;
  const json::Value* result = root.find("result");
  if (!result || !result->is_object()) return out;
  const json::Value* devices = result->find("devices");
  if (!devices || !devices->is_array()) return out;

  for (const auto& d : devices->items()) {
    if (!d.is_object()) continue;
    const json::Value* hw = d.find("hardwareProperties");
    const json::Value* dp = d.find("deviceProperties");
    const json::Value* cp = d.find("connectionProperties");

    // Only iOS-family devices are targets for this adapter.
    const std::string platform = hw ? str_at(*hw, "platform") : std::string();
    if (platform != "iOS" && platform != "iPadOS" && platform != "xrOS" &&
        platform != "tvOS" && platform != "watchOS") {
      continue;
    }

    model::DeviceRef ref;
    ref.platform = model::Platform::kIos;
    // `identifier` is the CoreDevice UUID and is the handle every other
    // devicectl command accepts, so it is our stable device_id.
    ref.device_id = str_at(d, "identifier");
    if (ref.device_id.empty() && hw) ref.device_id = str_at(*hw, "udid");
    if (ref.device_id.empty()) continue;
    // ...and the hardware UDID alongside it, because xctrace knows the device
    // by that and by nothing else. See DeviceRef::hardware_udid: passing the
    // CoreDevice id to xctrace failed every physical-device capture.
    if (hw) {
      const std::string udid = str_at(*hw, "udid");
      if (!udid.empty() && udid != ref.device_id) ref.hardware_udid = udid;
    }

    if (hw) {
      ref.model = str_at(*hw, "marketingName");
      if (ref.model.empty()) ref.model = str_at(*hw, "productType");
      const std::string reality = str_at(*hw, "reality");
      // "physical" is the value observed; anything else is not assumed to be
      // physical.
      ref.form = reality == "physical" ? model::DeviceForm::kPhysical
                 : reality == "simulated" ? model::DeviceForm::kSimulator
                                          : model::DeviceForm::kUnknown;
    }
    if (dp) {
      ref.display_name = str_at(*dp, "name");
      ref.os_version = str_at(*dp, "osVersionNumber");
      const std::string build = str_at(*dp, "osBuildUpdate");
      if (!build.empty()) ref.os_version += " (" + build + ")";
    }
    if (ref.display_name.empty()) ref.display_name = ref.model;

    if (cp) {
      const std::string tunnel = str_at(*cp, "tunnelState");
      const std::string pairing = str_at(*cp, "pairingState");
      const std::string transport = str_at(*cp, "transportType");
      // A paired-but-unreachable device is offline, not authorized. Treating
      // it as usable would produce a capture attempt that cannot work.
      if (tunnel == "connected" || tunnel == "available") {
        ref.trust = pairing == "paired" ? model::TrustState::kAuthorized
                                        : model::TrustState::kUntrusted;
      } else if (pairing != "paired") {
        ref.trust = model::TrustState::kUntrusted;
      } else {
        ref.trust = model::TrustState::kOffline;
      }
      if (transport == "wired" || transport == "localHost") {
        ref.connection = model::ConnectionType::kUsb;
      } else if (transport == "localNetwork" || transport == "wifi") {
        ref.connection = model::ConnectionType::kWireless;
      } else {
        ref.connection = model::ConnectionType::kUnknown;
      }
    } else {
      ref.trust = model::TrustState::kUnknown;
    }

    // Developer mode disabled is a distinct blocker from an untrusted pairing.
    if (dp) {
      const std::string dev_mode = str_at(*dp, "developerModeStatus");
      if (!dev_mode.empty() && dev_mode != "enabled" &&
          ref.trust == model::TrustState::kAuthorized) {
        ref.trust = model::TrustState::kUntrusted;
      }
    }

    ref.provider = provider_version.empty() ? "devicectl"
                                            : "devicectl " + provider_version;
    ref.observed_at = iso_now();
    out.push_back(std::move(ref));
  }
  return out;
}

std::optional<DeviceReadiness> parse_devicectl_readiness(
    const json::Value& root, const std::string& identifier) {
  const json::Value* result = root.find("result");
  if (!result || !result->is_object()) return std::nullopt;
  const json::Value* devices = result->find("devices");
  if (!devices || !devices->is_array()) {
    // `device info details` returns a single device object instead.
    const json::Value* hw = result->find("hardwareProperties");
    if (!hw) return std::nullopt;
    DeviceReadiness r;
    if (const json::Value* dp = result->find("deviceProperties"); dp) {
      if (const json::Value* ddi = dp->find("ddiServicesAvailable"); ddi && ddi->is_bool()) {
        r.ddi_services_available = ddi->as_bool();
      }
      r.developer_mode_status = str_at(*dp, "developerModeStatus");
    }
    if (const json::Value* cp = result->find("connectionProperties"); cp) {
      r.tunnel_state = str_at(*cp, "tunnelState");
      r.pairing_state = str_at(*cp, "pairingState");
    }
    return r;
  }
  for (const auto& d : devices->items()) {
    if (!d.is_object()) continue;
    const json::Value* hw = d.find("hardwareProperties");
    if (str_at(d, "identifier") != identifier &&
        (!hw || str_at(*hw, "udid") != identifier)) {
      continue;
    }
    DeviceReadiness r;
    if (const json::Value* dp = d.find("deviceProperties"); dp) {
      if (const json::Value* ddi = dp->find("ddiServicesAvailable"); ddi && ddi->is_bool()) {
        r.ddi_services_available = ddi->as_bool();
      }
      r.developer_mode_status = str_at(*dp, "developerModeStatus");
    }
    if (const json::Value* cp = d.find("connectionProperties"); cp) {
      r.tunnel_state = str_at(*cp, "tunnelState");
      r.pairing_state = str_at(*cp, "pairingState");
    }
    return r;
  }
  return std::nullopt;
}

std::vector<InstalledApp> parse_devicectl_apps(const json::Value& root) {
  std::vector<InstalledApp> out;
  const json::Value* result = root.find("result");
  if (!result || !result->is_object()) return out;
  const json::Value* apps = result->find("apps");
  if (!apps || !apps->is_array()) return out;
  for (const auto& a : apps->items()) {
    if (!a.is_object()) continue;
    InstalledApp app;
    app.bundle_id = str_at(a, "bundleIdentifier");
    if (app.bundle_id.empty()) continue;
    app.name = str_at(a, "name");
    if (app.name.empty()) app.name = str_at(a, "bundleName");
    app.version = str_at(a, "version");
    if (app.version.empty()) app.version = str_at(a, "bundleVersion");
    // devicectl reports both an install type and, on recent versions, whether
    // the app is debuggable. Where the field is absent we leave the hint unset
    // rather than defaulting it.
    const json::Value* removable = a.find("removable");
    const std::string install_type = str_at(a, "installationType");
    if (!install_type.empty()) {
      app.app_type = install_type;
    } else if (removable && removable->is_bool()) {
      app.app_type = removable->as_bool() ? "User" : "System";
    }
    for (const char* key : {"debuggable", "isDebuggable", "profileable"}) {
      if (const json::Value* v = a.find(key); v && v->is_bool()) {
        app.profileable_hint = v->as_bool();
        break;
      }
    }
    out.push_back(std::move(app));
  }
  return out;
}

std::vector<DeviceProcess> parse_devicectl_processes(const json::Value& root) {
  std::vector<DeviceProcess> out;
  const json::Value* result = root.find("result");
  if (!result || !result->is_object()) return out;
  const json::Value* procs = result->find("runningProcesses");
  if (!procs || !procs->is_array()) {
    procs = result->find("processes");
  }
  if (!procs || !procs->is_array()) return out;
  for (const auto& p : procs->items()) {
    if (!p.is_object()) continue;
    DeviceProcess dp;
    const json::Value* pid = p.find("processIdentifier");
    if (!pid) pid = p.find("pid");
    if (!pid || !pid->is_number()) continue;
    dp.pid = static_cast<std::int32_t>(pid->as_int());
    dp.executable_path = str_at(p, "executable");
    if (dp.executable_path.empty()) dp.executable_path = str_at(p, "executablePath");
    out.push_back(std::move(dp));
  }
  return out;
}

std::vector<model::DeviceRef> parse_simctl_devices(
    const json::Value& root, const std::string& provider_version) {
  std::vector<model::DeviceRef> out;
  const json::Value* devices = root.find("devices");
  if (!devices || !devices->is_object()) return out;
  for (const auto& runtime : devices->members()) {
    if (!runtime.second.is_array()) continue;
    // Runtime key: "com.apple.CoreSimulator.SimRuntime.iOS-26-5".
    std::string os_version;
    const std::string& key = runtime.first;
    const std::size_t ios_pos = key.find("iOS-");
    if (ios_pos != std::string::npos) {
      os_version = key.substr(ios_pos + 4);
      std::replace(os_version.begin(), os_version.end(), '-', '.');
    } else {
      // Not an iOS runtime (watchOS, tvOS, xrOS): out of scope here.
      continue;
    }
    for (const auto& d : runtime.second.items()) {
      if (!d.is_object()) continue;
      const json::Value* available = d.find("isAvailable");
      if (available && available->is_bool() && !available->as_bool()) continue;
      model::DeviceRef ref;
      ref.platform = model::Platform::kIos;
      ref.device_id = str_at(d, "udid");
      if (ref.device_id.empty()) continue;
      ref.display_name = str_at(d, "name");
      ref.model = str_at(d, "deviceTypeIdentifier");
      ref.os_version = os_version;
      // Never conflated with a physical device (spec 4.1, J18).
      ref.form = model::DeviceForm::kSimulator;
      ref.connection = model::ConnectionType::kLocal;
      // Only a booted simulator can be inspected.
      ref.trust = str_at(d, "state") == "Booted" ? model::TrustState::kAuthorized
                                                 : model::TrustState::kOffline;
      ref.provider = provider_version.empty() ? "simctl"
                                              : "simctl " + provider_version;
      ref.observed_at = iso_now();
      // A simulator's identity is invalidated by an erase, which also resets
      // its data path; the last-booted stamp is the closest available proxy
      // for a boot session.
      ref.boot_id = str_at(d, "lastBootedAt");
      out.push_back(std::move(ref));
    }
  }
  return out;
}

std::vector<InstalledApp> parse_simctl_listapps(const json::Value& root) {
  std::vector<InstalledApp> out;
  if (!root.is_object()) return out;
  // The converted plist is a map of bundle id -> app dictionary.
  for (const auto& kv : root.members()) {
    if (!kv.second.is_object()) continue;
    InstalledApp app;
    app.bundle_id = str_at(kv.second, "CFBundleIdentifier");
    if (app.bundle_id.empty()) app.bundle_id = kv.first;
    app.name = str_at(kv.second, "CFBundleDisplayName");
    if (app.name.empty()) app.name = str_at(kv.second, "CFBundleName");
    app.version = str_at(kv.second, "CFBundleVersion");
    app.app_type = str_at(kv.second, "ApplicationType");
    out.push_back(std::move(app));
  }
  std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) {
    return a.bundle_id < b.bundle_id;
  });
  return out;
}

std::vector<LaunchdEntry> parse_launchctl_list(const std::string& text) {
  std::vector<LaunchdEntry> out;
  std::istringstream ss(text);
  std::string line;
  bool first = true;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      // Skip the "PID\tStatus\tLabel" header.
      if (line.rfind("PID", 0) == 0) continue;
    }
    if (line.empty()) continue;
    // Tab-separated: PID, Status, Label. PID is "-" for a job that is loaded
    // but not running.
    std::vector<std::string> cols;
    std::size_t start = 0;
    for (;;) {
      const std::size_t tab = line.find('\t', start);
      cols.push_back(line.substr(start, tab == std::string::npos
                                            ? std::string::npos
                                            : tab - start));
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    if (cols.size() < 3) continue;
    LaunchdEntry e;
    const std::string pid_s = trim(cols[0]);
    if (pid_s != "-" && !pid_s.empty() &&
        pid_s.find_first_not_of("0123456789") == std::string::npos) {
      e.pid = static_cast<std::int32_t>(std::atoll(pid_s.c_str()));
    }
    e.label = trim(cols[2]);
    // "UIKitApplication:com.example.app[079d][rb-legacy]" -- launchd itself
    // attributes this process to the bundle id.
    const std::string prefix = "UIKitApplication:";
    if (e.label.rfind(prefix, 0) == 0) {
      const std::string rest = e.label.substr(prefix.size());
      const std::size_t bracket = rest.find('[');
      e.bundle_id = bracket == std::string::npos ? rest : rest.substr(0, bracket);
    }
    out.push_back(std::move(e));
  }
  return out;
}

IosAdapter::IosAdapter() = default;

std::string IosAdapter::devicectl_version(const discovery::ProviderOptions& opts) const {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;
  const auto r = proc::run({"xcrun", "devicectl", "--version"}, po);
  return r.ok() ? trim(r.out) : std::string();
}

std::string IosAdapter::xctrace_version(const discovery::ProviderOptions& opts) const {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;
  const auto r = proc::run({"xcrun", "xctrace", "version"}, po);
  if (!r.ok()) return {};
  std::istringstream ss(r.out);
  std::string line;
  std::getline(ss, line);
  return trim(line);
}

std::optional<json::Value> IosAdapter::run_devicectl_json(
    const std::vector<std::string>& args, const discovery::ProviderOptions& opts,
    std::string* error) const {
  TempJsonFile tmp;
  if (!tmp.valid()) {
    if (error) *error = "could not create a temporary file for devicectl output";
    return std::nullopt;
  }
  std::vector<std::string> argv{"xcrun", "devicectl"};
  for (const auto& a : args) {
    if (!proc::is_safe_argument(a, /*reject_option_like=*/false)) {
      if (error) *error = "refusing an unsafe devicectl argument";
      return std::nullopt;
    }
    argv.push_back(a);
  }
  // devicectl's own help states this file is the only supported machine
  // interface, so we never parse its stdout.
  argv.push_back("--json-output");
  argv.push_back(tmp.path());

  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;
  const auto r = proc::run(argv, po);
  if (!r.spawned) {
    if (error) *error = "could not start xcrun devicectl: " + r.spawn_error;
    return std::nullopt;
  }
  if (r.timed_out) {
    if (error) {
      *error = "devicectl timed out after " +
               std::to_string(opts.command_timeout_ms) + " ms";
    }
    return std::nullopt;
  }
  json::ParseError perr;
  auto parsed = json::parse_file(tmp.path(), json::Limits{}, &perr);
  if (!parsed) {
    if (error) {
      *error = r.exit_code != 0
                   ? ("devicectl exited " + std::to_string(r.exit_code) + ": " +
                      trim(r.err))
                   : ("devicectl produced no parseable JSON: " + perr.message);
    }
    return std::nullopt;
  }
  if (r.exit_code != 0) {
    // devicectl writes a structured error document; surface it and let the
    // caller decide, rather than treating a failure as an empty result.
    if (error) {
      const json::Value* err_obj = parsed->find("error");
      std::string detail;
      if (err_obj && err_obj->is_object()) {
        detail = str_at(*err_obj, "userInfo");
        if (detail.empty()) detail = str_at(*err_obj, "localizedDescription");
        if (detail.empty()) detail = str_at(*err_obj, "description");
      }
      *error = "devicectl exited " + std::to_string(r.exit_code) +
               (detail.empty() ? (": " + trim(r.err)) : (": " + detail));
    }
    return std::nullopt;
  }
  return parsed;
}

void IosAdapter::probe(model::CapabilityMatrix& out,
                       const discovery::ProviderOptions& opts) const {
  // xcrun is the gate for everything else.
  if (!proc::which("xcrun").has_value()) {
    auto c = make_cap("ios.toolchain.xcrun", "Xcode command-line tools",
                      model::CapabilityStatus::kUnsupported, "xcrun");
    c.evidence = "`xcrun` was not found on PATH";
    c.prerequisites.push_back("Xcode installed and selected via xcode-select");
    c.recovery_action =
        "install Xcode from the App Store and run `sudo xcode-select -s "
        "/Applications/Xcode.app`";
    c.tested = model::TestedState::kProbedOnly;
    out.upsert(std::move(c));
    for (const auto* id : {"ios.discovery.devices", "ios.discovery.installed_apps",
                           "ios.discovery.running_processes", "ios.capture.attach"}) {
      auto d = make_cap(id, id, model::CapabilityStatus::kUnsupported, "xcrun");
      d.evidence = "depends on the Xcode command-line tools, which are absent";
      d.tested = model::TestedState::kProbedOnly;
      out.upsert(std::move(d));
    }
    return;
  }

  const std::string dc_version = devicectl_version(opts);
  const std::string xt_version = xctrace_version(opts);
  {
    auto c = make_cap("ios.toolchain.xcrun", "Xcode command-line tools",
                      model::CapabilityStatus::kAvailable, "xcrun");
    proc::Options po;
    po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
    po.cancel = opts.cancel;
    const auto sel = proc::run({"xcode-select", "-p"}, po);
    c.evidence = "xcode-select -p => " + trim(sel.out) +
                 "; devicectl " + (dc_version.empty() ? "absent" : dc_version) +
                 "; " + (xt_version.empty() ? "xctrace absent" : xt_version);
    c.tested = model::TestedState::kProbedOnly;
    out.upsert(std::move(c));
  }

  std::vector<std::string> errors;
  const auto devices = list_devices(opts, errors);
  std::size_t physical = 0, simulators = 0, usable_physical = 0, booted_sims = 0;
  for (const auto& d : devices) {
    if (d.form == model::DeviceForm::kSimulator) {
      ++simulators;
      if (d.usable_for_capture()) ++booted_sims;
    } else {
      ++physical;
      if (d.usable_for_capture()) ++usable_physical;
    }
  }

  {
    auto c = make_cap("ios.discovery.devices", "iOS device discovery",
                      model::CapabilityStatus::kAvailable, "devicectl + simctl");
    c.provider_version = dc_version;
    c.evidence = "`devicectl list devices --json-output` reported " +
                 std::to_string(physical) + " physical device(s) (" +
                 std::to_string(usable_physical) +
                 " reachable); `simctl list devices --json` reported " +
                 std::to_string(simulators) + " available simulator(s) (" +
                 std::to_string(booted_sims) + " booted)";
    c.scope =
        "devices paired with this host, plus local simulators. Simulators are "
        "reported separately and are never treated as physical devices.";
    if (usable_physical > 0) {
      c.tested = model::TestedState::kVerifiedOnPhysicalDevice;
    } else if (booted_sims > 0) {
      c.tested = model::TestedState::kVerifiedOnSimulatorOrEmulator;
      c.limitations.push_back(
          "verified against simulators only: no physical iOS device was "
          "reachable at probe time");
    } else {
      c.tested = model::TestedState::kProbedOnly;
    }
    if (physical > 0 && usable_physical == 0) {
      c.limitations.push_back(
          std::to_string(physical) +
          " physical device(s) are paired but unreachable (tunnelState is not "
          "connected); they are reported as offline, not as absent");
    }
    for (const auto& e : errors) c.limitations.push_back(e);
    out.upsert(std::move(c));
  }

  // Installed-app enumeration and running-process enumeration are validated
  // independently (spec 3.4), and separately for physical vs simulator.
  const model::DeviceRef* phys = nullptr;
  const model::DeviceRef* sim = nullptr;
  for (const auto& d : devices) {
    if (d.form == model::DeviceForm::kSimulator) {
      if (!sim && d.usable_for_capture()) sim = &d;
    } else if (!phys && d.usable_for_capture()) {
      phys = &d;
    }
  }

  {
    auto c = make_cap("ios.discovery.installed_apps",
                      "Installed app enumeration (physical device)",
                      model::CapabilityStatus::kUnknown, "devicectl device info apps");
    c.provider_version = dc_version;
    c.prerequisites.push_back(
        "a reachable, paired physical device with Developer Mode enabled");
    c.prerequisites.push_back(
        "Developer Disk Image services mounted (ddiServicesAvailable = true)");
    if (!phys) {
      c.evidence =
          physical == 0
              ? "no physical iOS device is paired with this host, so this "
                "could not be probed"
              : "every paired physical device is unreachable "
                "(connectionProperties.tunnelState != connected)";
      c.recovery_action =
          "connect and unlock a trusted iPhone or iPad with Developer Mode "
          "enabled, then re-run discovery";
      c.tested = model::TestedState::kNotTested;
    } else {
      std::string err;
      auto details = run_devicectl_json({"device", "info", "details", "--device",
                                         phys->device_id},
                                        opts, &err);
      std::optional<DeviceReadiness> readiness;
      if (details) readiness = parse_devicectl_readiness(*details, phys->device_id);
      if (readiness && readiness->ddi_services_available.has_value() &&
          !*readiness->ddi_services_available) {
        c.status = model::CapabilityStatus::kPermissionDenied;
        c.evidence =
            "the device reports ddiServicesAvailable = false, so app and "
            "process enumeration services are not mounted";
        c.recovery_action =
            "open Xcode with the device connected so it prepares the "
            "developer disk image, then retry";
        c.tested = model::TestedState::kProbedOnly;
      } else {
        auto apps = run_devicectl_json(
            {"device", "info", "apps", "--device", phys->device_id}, opts, &err);
        if (apps) {
          const auto list = parse_devicectl_apps(*apps);
          c.status = list.empty() ? model::CapabilityStatus::kLimited
                                  : model::CapabilityStatus::kAvailable;
          c.evidence = "`devicectl device info apps` returned " +
                       std::to_string(list.size()) + " app(s)";
          c.tested = model::TestedState::kVerifiedOnPhysicalDevice;
          c.limitations.push_back(
              "installed does not mean running, and an App Store app is "
              "generally not deeply profileable");
        } else {
          c.status = model::CapabilityStatus::kUnsupported;
          c.evidence = err;
          c.tested = model::TestedState::kProbedOnly;
        }
      }
    }
    out.upsert(std::move(c));
  }

  {
    auto c = make_cap("ios.discovery.running_processes",
                      "Running process enumeration (physical device)",
                      model::CapabilityStatus::kUnknown,
                      "devicectl device info processes");
    c.provider_version = dc_version;
    c.prerequisites.push_back("a reachable physical device with DDI services");
    if (!phys) {
      c.evidence = "no reachable physical iOS device; not probed";
      c.recovery_action = "connect and unlock a trusted device";
      c.tested = model::TestedState::kNotTested;
      c.limitations.push_back(
          "iOS does not expose a general foreground/suspended signal, so even "
          "when this works the runtime state of an app is reported as running "
          "or unknown -- never guessed as suspended");
    } else {
      std::string err;
      auto procs = run_devicectl_json(
          {"device", "info", "processes", "--device", phys->device_id}, opts, &err);
      if (procs) {
        const auto list = parse_devicectl_processes(*procs);
        c.status = list.empty() ? model::CapabilityStatus::kLimited
                                : model::CapabilityStatus::kAvailable;
        c.evidence = "`devicectl device info processes` returned " +
                     std::to_string(list.size()) + " process(es)";
        c.tested = model::TestedState::kVerifiedOnPhysicalDevice;
        c.limitations.push_back(
            "processes are reported by executable path; mapping a path to a "
            "bundle id requires the installed-app listing, and an unmatched "
            "path stays unassigned");
      } else {
        c.status = model::CapabilityStatus::kUnsupported;
        c.evidence = err;
        c.tested = model::TestedState::kProbedOnly;
      }
    }
    out.upsert(std::move(c));
  }

  {
    auto c = make_cap("ios.discovery.simulator_apps",
                      "Installed and running app enumeration (simulator)",
                      model::CapabilityStatus::kUnknown, "simctl");
    c.prerequisites.push_back("a booted iOS simulator");
    c.prerequisites.push_back(
        "`plutil` to convert simctl's NeXTSTEP plist output to JSON");
    if (!sim) {
      c.evidence = "no booted iOS simulator was found; not probed";
      c.recovery_action = "boot a simulator (`xcrun simctl boot <udid>`)";
      c.tested = model::TestedState::kNotTested;
    } else {
      std::vector<std::string> errs;
      bool failed = false;
      const auto apps = list_apps(*sim, opts, errs, failed);
      std::size_t running = 0;
      for (const auto& a : apps) {
        if (a.runtime_state == model::RuntimeState::kRunning) ++running;
      }
      c.status = failed ? model::CapabilityStatus::kUnsupported
                 : apps.empty() ? model::CapabilityStatus::kLimited
                                : model::CapabilityStatus::kAvailable;
      c.evidence = "simulator " + sim->device_id + ": " +
                   std::to_string(apps.size()) + " app(s) listed, " +
                   std::to_string(running) +
                   " with a launchd-attributed running process";
      c.tested = model::TestedState::kVerifiedOnSimulatorOrEmulator;
      c.scope =
          "simulator only. Simulator timing is never comparable to a physical "
          "device and is kept in a separate baseline (spec J18).";
      for (const auto& e : errs) c.limitations.push_back(e);
    }
    out.upsert(std::move(c));
  }

  {
    // Deep attach is the capability the specification is most insistent must
    // not be over-claimed.
    auto c = make_cap("ios.capture.attach",
                      "Permission to attach a profiler to a selected app",
                      model::CapabilityStatus::kUnknown, "xctrace");
    c.provider_version = xt_version;
    c.prerequisites.push_back(
        "the app is signed with a development profile by this developer, or "
        "carries get-task-allow / the debugger entitlement");
    c.prerequisites.push_back("a reachable device with Developer Mode enabled");
    c.evidence =
        "not determinable at the device level: it depends on the selected "
        "app's code signature and entitlements. It is re-probed per app during "
        "preflight.";
    c.scope =
        "an app the developer builds and signs. A third-party App Store app "
        "generally cannot be deeply attached, and this tool does not attempt "
        "to bypass that.";
    c.recovery_action =
        "build and install the target from Xcode with a development signing "
        "identity, then retry";
    c.limitations.push_back(
        "no jailbreak, private API, or entitlement bypass is used or offered");
    c.tested = model::TestedState::kNotTested;
    out.upsert(std::move(c));
  }

  {
    auto c = make_cap("ios.capture.live_recording",
                      "Live trace capture from a physical iOS device",
                      model::CapabilityStatus::kUnknown, "xctrace");
    c.provider_version = xt_version;
    c.prerequisites.push_back("a reachable physical device");
    c.prerequisites.push_back("an attachable, developer-signed app");
    c.evidence =
        xt_version.empty()
            ? "xctrace is not available on this host"
            : xt_version +
                  " is installed, but live capture has NOT been exercised "
                  "against a physical device in this environment";
    c.limitations.push_back(
        "UNVERIFIED: no physical iOS device was reachable during "
        "implementation, so the live capture path is implemented but not "
        "demonstrated. This blocks the M2 gate for iOS.");
    c.recovery_action =
        "connect a trusted physical iPhone or iPad and run `mpi preflight` to "
        "convert this from unknown to a measured result";
    c.tested = model::TestedState::kNotTested;
    out.upsert(std::move(c));
  }
}

std::vector<model::DeviceRef> IosAdapter::list_devices(
    const discovery::ProviderOptions& opts, std::vector<std::string>& errors) const {
  std::vector<model::DeviceRef> out;
  if (!proc::which("xcrun").has_value()) {
    errors.push_back("xcrun not found; iOS devices cannot be enumerated");
    return out;
  }

  const std::string dc_version = devicectl_version(opts);
  std::string err;
  if (auto doc = run_devicectl_json({"list", "devices"}, opts, &err)) {
    for (auto& d : parse_devicectl_devices(*doc, dc_version)) {
      out.push_back(std::move(d));
    }
  } else {
    errors.push_back("devicectl device listing failed: " + err);
  }

  if (include_simulators_) {
    proc::Options po;
    po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
    po.cancel = opts.cancel;
    const auto r = proc::run({"xcrun", "simctl", "list", "devices", "--json"}, po);
    if (r.ok()) {
      json::ParseError perr;
      if (auto doc = json::parse(r.out, json::Limits{}, &perr)) {
        for (auto& d : parse_simctl_devices(*doc, "")) out.push_back(std::move(d));
      } else {
        errors.push_back("simctl JSON parse failed: " + perr.message);
      }
    } else {
      errors.push_back("`simctl list devices --json` failed: " +
                       (r.spawned ? trim(r.err) : r.spawn_error));
    }
  }
  return out;
}

std::vector<model::AppEntry> IosAdapter::list_apps(
    const model::DeviceRef& device, const discovery::ProviderOptions& opts,
    std::vector<std::string>& errors, bool& enumeration_failed) const {
  enumeration_failed = false;
  std::vector<model::AppEntry> out;
  const std::string observed = iso_now();

  if (!device.usable_for_capture()) {
    errors.push_back(
        "device " + device.device_id + " is " + model::to_string(device.trust) +
        "; apps cannot be enumerated. This is a reachability problem, not an "
        "empty app list.");
    enumeration_failed = true;
    return out;
  }

  proc::Options po;
  po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
  po.cancel = opts.cancel;

  if (device.form == model::DeviceForm::kSimulator) {
    // simctl listapps emits an old-style plist, so it is converted first.
    const auto listed = proc::run(
        {"xcrun", "simctl", "listapps", device.device_id}, po);
    if (!listed.ok()) {
      errors.push_back("`simctl listapps` failed: " +
                       (listed.spawned ? trim(listed.err) : listed.spawn_error));
      enumeration_failed = true;
      return out;
    }
    // Convert via plutil by writing the plist to a temp file: plutil reads
    // stdin only with "-", and we avoid any shell pipeline.
    TempJsonFile plist_tmp;
    if (!plist_tmp.valid()) {
      errors.push_back("could not create a temporary file for plist conversion");
      enumeration_failed = true;
      return out;
    }
    {
      std::ofstream f(plist_tmp.path(), std::ios::binary | std::ios::trunc);
      f.write(listed.out.data(), static_cast<std::streamsize>(listed.out.size()));
    }
    TempJsonFile json_tmp;
    const auto conv = proc::run({"plutil", "-convert", "json", "-r", "-o",
                                 json_tmp.path(), plist_tmp.path()}, po);
    if (!conv.ok()) {
      errors.push_back(
          "`plutil -convert json` failed on simctl listapps output: " +
          (conv.spawned ? trim(conv.err) : conv.spawn_error));
      enumeration_failed = true;
      return out;
    }
    json::ParseError perr;
    auto doc = json::parse_file(json_tmp.path(), json::Limits{}, &perr);
    if (!doc) {
      errors.push_back("converted simctl listapps JSON did not parse: " + perr.message);
      enumeration_failed = true;
      return out;
    }
    const auto installed = parse_simctl_listapps(*doc);

    // launchd is the authority on which bundle ids are running.
    std::vector<LaunchdEntry> launchd;
    const auto lc = proc::run(
        {"xcrun", "simctl", "spawn", device.device_id, "launchctl", "list"}, po);
    bool launchd_ok = false;
    if (lc.ok()) {
      launchd = parse_launchctl_list(lc.out);
      launchd_ok = !launchd.empty();
    } else {
      errors.push_back(
          "`simctl spawn launchctl list` failed, so runtime state stays "
          "unknown: " + (lc.spawned ? trim(lc.err) : lc.spawn_error));
    }

    for (const auto& app : installed) {
      model::AppEntry e;
      e.key.platform = model::Platform::kIos;
      e.key.device_id = device.device_id;
      e.key.app_identifier = app.bundle_id;
      e.key.identifier_kind = model::IdentifierKind::kBundleId;
      e.display_name = app.name.empty() ? app.bundle_id : app.name;
      e.installed_known = true;
      e.installed = true;
      e.provider = "simctl listapps";
      e.observed_at = observed;
      e.visibility_scope = model::DiscoveryScope::kCompleteForProvider;

      if (!launchd_ok) {
        e.runtime_state = model::RuntimeState::kUnknown;
        e.notes.push_back(
            "launchd could not be queried, so the runtime state is unknown "
            "rather than not running");
      } else {
        for (const auto& l : launchd) {
          if (l.bundle_id != app.bundle_id) continue;
          if (!l.pid.has_value()) continue;  // loaded but not running
          model::ProcessInstance p;
          p.app = e.key;
          p.pid = *l.pid;
          p.process_name = l.label;
          p.is_primary = true;
          p.boot_id = device.boot_id;
          // launchd's own label attributes this process to the bundle id.
          p.ownership = model::OwnershipEvidence::kProviderAttributed;
          p.ownership_note =
              "launchd job label '" + l.label +
              "' names this bundle id, which is the provider attributing the "
              "process to the app";
          // No start time is available from launchctl list, so PID reuse
          // cannot be excluded; that is stated rather than glossed over.
          p.ownership_note +=
              "; launchctl list reports no process start time, so PID reuse "
              "across the capture cannot be excluded from this source alone";
          e.processes.push_back(std::move(p));
        }
        e.runtime_state = e.processes.empty() ? model::RuntimeState::kNotRunning
                                              : model::RuntimeState::kRunning;
        if (!e.processes.empty()) {
          e.notes.push_back(
              "running; iOS exposes no general foreground/suspended signal, so "
              "foreground state is not claimed");
        }
      }

      // A simulator run can be profiled, but it is not a device measurement.
      if (app.app_type == "System") {
        e.profiling = model::ProfilingAvailability::kUnavailable;
        e.profiling_reason =
            "a system app in the simulator runtime is not a profiling target "
            "for this tool";
      } else {
        e.profiling = model::ProfilingAvailability::kLimited;
        e.profiling_reason =
            "simulator only: measurements do not represent physical-device "
            "performance and must not be compared against device baselines";
        e.profiling_recovery_action =
            "for production-like numbers, install and profile on a physical "
            "device";
      }
      e.notes.push_back("bundle type: " +
                        (app.app_type.empty() ? std::string("unknown") : app.app_type));
      out.push_back(std::move(e));
    }

    // A launchd-running bundle id that is not in the installed listing is
    // reported rather than dropped.
    if (launchd_ok) {
      for (const auto& l : launchd) {
        if (l.bundle_id.empty() || !l.pid.has_value()) continue;
        const bool known = std::any_of(
            installed.begin(), installed.end(), [&](const InstalledApp& a) {
              return a.bundle_id == l.bundle_id;
            });
        if (known) continue;
        model::AppEntry e;
        e.key.platform = model::Platform::kIos;
        e.key.device_id = device.device_id;
        e.key.app_identifier = l.bundle_id;
        e.key.identifier_kind = model::IdentifierKind::kBundleId;
        e.display_name = l.bundle_id;
        e.installed_known = true;
        e.installed = false;
        e.runtime_state = model::RuntimeState::kRunning;
        e.visibility_scope = model::DiscoveryScope::kPartial;
        e.provider = "simctl spawn launchctl list";
        e.observed_at = observed;
        e.profiling = model::ProfilingAvailability::kUnknown;
        e.profiling_reason =
            "a launchd job names this bundle id but it is absent from the "
            "installed-app listing";
        e.notes.push_back("seen only via launchd; not in `simctl listapps`");
        model::ProcessInstance p;
        p.app = e.key;
        p.pid = *l.pid;
        p.process_name = l.label;
        p.boot_id = device.boot_id;
        p.ownership = model::OwnershipEvidence::kProviderAttributed;
        p.ownership_note = "launchd label names this bundle id";
        e.processes.push_back(std::move(p));
        out.push_back(std::move(e));
      }
    }

    std::sort(out.begin(), out.end(),
              [](const model::AppEntry& a, const model::AppEntry& b) {
                return a.key.app_identifier < b.key.app_identifier;
              });
    return out;
  }

  // Physical device path.
  std::string err;
  auto details = run_devicectl_json(
      {"device", "info", "details", "--device", device.device_id}, opts, &err);
  std::optional<DeviceReadiness> readiness;
  if (details) readiness = parse_devicectl_readiness(*details, device.device_id);
  if (readiness && readiness->ddi_services_available.has_value() &&
      !*readiness->ddi_services_available) {
    errors.push_back(
        "device reports ddiServicesAvailable = false: the developer disk image "
        "services are not mounted, so apps cannot be enumerated. Open Xcode "
        "with the device connected to prepare it.");
    enumeration_failed = true;
    return out;
  }

  auto apps_doc = run_devicectl_json(
      {"device", "info", "apps", "--device", device.device_id}, opts, &err);
  if (!apps_doc) {
    errors.push_back("`devicectl device info apps` failed: " + err);
    enumeration_failed = true;
    return out;
  }
  const auto installed = parse_devicectl_apps(*apps_doc);

  // Process enumeration is a separate capability and may fail on its own.
  std::vector<DeviceProcess> processes;
  bool processes_ok = false;
  {
    std::string perr_msg;
    if (auto procs_doc = run_devicectl_json(
            {"device", "info", "processes", "--device", device.device_id}, opts,
            &perr_msg)) {
      processes = parse_devicectl_processes(*procs_doc);
      processes_ok = true;
    } else {
      errors.push_back(
          "`devicectl device info processes` failed, so runtime state stays "
          "unknown: " + perr_msg);
    }
  }

  for (const auto& app : installed) {
    model::AppEntry e;
    e.key.platform = model::Platform::kIos;
    e.key.device_id = device.device_id;
    e.key.app_identifier = app.bundle_id;
    e.key.identifier_kind = model::IdentifierKind::kBundleId;
    e.display_name = app.name.empty() ? app.bundle_id : app.name;
    e.installed_known = true;
    e.installed = true;
    e.provider = "devicectl device info apps";
    e.observed_at = observed;
    e.visibility_scope = model::DiscoveryScope::kCompleteForProvider;

    if (!processes_ok) {
      // Spec A11: unknown must never render as not_running.
      e.runtime_state = model::RuntimeState::kUnknown;
      e.notes.push_back(
          "process enumeration was unavailable, so runtime state is unknown");
    } else {
      // A process is matched to a bundle only by its executable path
      // containing the app's own bundle directory. A display-name match is
      // never used (spec 3.4).
      for (const auto& p : processes) {
        if (p.executable_path.empty()) continue;
        const std::string needle = "/" + app.bundle_id + "/";
        const bool path_names_bundle =
            p.executable_path.find(needle) != std::string::npos;
        if (!path_names_bundle) continue;
        model::ProcessInstance pi;
        pi.app = e.key;
        pi.pid = p.pid;
        pi.process_name = p.executable_path;
        pi.is_primary = true;
        pi.boot_id = device.boot_id;
        // The path containing the bundle id is suggestive but not a provider
        // attribution: an extension or a helper under the same container
        // would match too. It stays ambiguous unless proven otherwise.
        pi.ownership = model::OwnershipEvidence::kAmbiguous;
        pi.ownership_note =
            "matched because the executable path contains the bundle id; "
            "devicectl does not attribute processes to bundle ids directly, so "
            "an app extension or helper under the same container could also "
            "match. Ownership is left unestablished.";
        e.processes.push_back(std::move(pi));
      }
      e.runtime_state = e.processes.empty() ? model::RuntimeState::kNotRunning
                                            : model::RuntimeState::kRunning;
      if (!e.processes.empty()) {
        e.notes.push_back(
            "running; iOS exposes no general foreground/suspended signal");
      }
    }

    // App type governs whether deep profiling is even plausible.
    if (app.app_type == "System" || app.app_type == "system") {
      e.profiling = model::ProfilingAvailability::kUnavailable;
      e.profiling_reason = "a system app cannot be attached to by this tool";
    } else if (app.profileable_hint.has_value() && *app.profileable_hint) {
      e.profiling = model::ProfilingAvailability::kAvailable;
      e.profiling_reason = "the device reports this app as debuggable";
    } else if (app.profileable_hint.has_value()) {
      e.profiling = model::ProfilingAvailability::kPermissionRequired;
      e.profiling_reason =
          "the device reports this app as not debuggable: it is most likely an "
          "App Store or distribution build";
      e.profiling_recovery_action =
          "install a development-signed build of this app from Xcode";
    } else {
      e.profiling = model::ProfilingAvailability::kUnknown;
      e.profiling_reason =
          "this toolchain did not report a debuggable flag for the app; "
          "attachability is resolved during preflight";
      e.profiling_recovery_action =
          "run `mpi preflight` for this bundle id to probe attachability";
    }
    if (!app.version.empty()) e.notes.push_back("version " + app.version);
    out.push_back(std::move(e));
  }

  // Processes that matched no installed app stay visible and unassigned.
  if (processes_ok) {
    for (const auto& p : processes) {
      bool matched = false;
      for (const auto& e : out) {
        for (const auto& pi : e.processes) {
          if (pi.pid == p.pid) {
            matched = true;
            break;
          }
        }
        if (matched) break;
      }
      if (matched) continue;
      // Only surface things that look like app containers, not the whole
      // system process table.
      if (p.executable_path.find("/Application") == std::string::npos) continue;
      errors.push_back("process " + std::to_string(p.pid) + " (" +
                       p.executable_path +
                       ") matched no installed bundle id and is left "
                       "unassigned");
    }
  }

  std::sort(out.begin(), out.end(),
            [](const model::AppEntry& a, const model::AppEntry& b) {
              return a.key.app_identifier < b.key.app_identifier;
            });
  return out;
}

std::vector<model::ProcessInstance> IosAdapter::resolve_processes(
    const model::DeviceRef& device, const model::ApplicationKey& app,
    const discovery::ProviderOptions& opts, std::vector<std::string>& errors) const {
  if (!proc::is_safe_argument(app.app_identifier, /*reject_option_like=*/true)) {
    errors.push_back("refusing to query the identifier '" + app.app_identifier +
                     "': it would be interpreted as a command-line option");
    return {};
  }
  bool failed = false;
  const auto apps = list_apps(device, opts, errors, failed);
  for (const auto& e : apps) {
    if (e.key.app_identifier == app.app_identifier) return e.processes;
  }
  if (!failed) {
    errors.push_back("no process instance of '" + app.app_identifier +
                     "' was observed on " + device.device_id + " at " + iso_now());
  }
  return {};
}

}  // namespace mpi::ios
