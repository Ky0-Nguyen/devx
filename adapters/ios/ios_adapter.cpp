#include "adapters/ios/ios_adapter.hpp"

#include "adapters/ios/xctrace_collector.hpp"

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
      // Recorded for the same reason `hardware_udid` is: this listing is a
      // cached record, and a property read from it is a description of the
      // device as it was when last seen.
      ref.last_seen_at = str_at(*cp, "lastConnectionDate");
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

std::string ddi_refusal_text(const DeviceReadiness& readiness) {
  std::string text =
      "device reports ddiServicesAvailable = false: the developer disk image "
      "services are not mounted, so apps cannot be enumerated. Open Xcode "
      "with the device connected to prepare it.";
  if (!readiness.last_connection_date.empty()) {
    // The reading's own date. `devicectl` answers from a cached record, so a
    // value with no date is a claim with no timestamp -- and this particular
    // claim sends someone to go and do something.
    text += " This was devicectl's reading as of " +
            readiness.last_connection_date +
            "; if the device has been reconnected since, re-run discovery "
            "before acting on it.";
  }
  if (!readiness.developer_mode_status.empty() &&
      readiness.developer_mode_status != "enabled") {
    // A distinct blocker, and worth naming here: someone told only about the
    // disk image will go to Xcode when the device is asking for a setting.
    text += " Developer Mode also reads as '" +
            readiness.developer_mode_status +
            "', which is a separate prerequisite: enable Settings > Privacy "
            "& Security > Developer Mode on the device.";
  }
  return text;
}

bool executable_path_names_bundle(const std::string& executable_path,
                                  const std::string& bundle_id) {
  if (executable_path.empty() || bundle_id.empty()) return false;
  std::size_t at = 0;
  while ((at = executable_path.find(bundle_id, at)) != std::string::npos) {
    // Must start a path segment, or a different bundle id ending in this one
    // would match -- "com.other.io.example" contains "io.example".
    const bool starts_segment = at == 0 || executable_path[at - 1] == '/';
    const std::size_t after = at + bundle_id.size();
    // And must end at a boundary the installers actually produce: the end of
    // the string, a path separator, `.app`, or the `-<digits>` a simulator
    // install appends. A bare alphanumeric after it means a *different*
    // identifier that happens to share this prefix.
    const bool ends_cleanly =
        after >= executable_path.size() || executable_path[after] == '/' ||
        executable_path[after] == '.' || executable_path[after] == '-';
    if (starts_segment && ends_cleanly) return true;
    at = after;
  }
  return false;
}

std::string describe_devicectl_failure(const std::string& raw) {
  // Matched on the error numbers as well as the sentences, since Apple can
  // reword the sentences and has.
  if (raw.find("error 1011") != std::string::npos ||
      raw.find("unable to locate a device") != std::string::npos) {
    return "the device is no longer there: it was present when discovery ran "
           "and CoreDevice could not find it for this operation, which is "
           "what unplugging a phone mid-session looks like. Re-run discovery. "
           "(devicectl: " + trim(raw) + ")";
  }
  if (raw.find("error 1000") != std::string::npos ||
      raw.find("specified device was not found") != std::string::npos) {
    return "devicectl does not recognise this device identifier at all, "
           "which is what a simulator UDID or a mistyped id gives -- it is "
           "not a statement about any phone. (devicectl: " + trim(raw) + ")";
  }
  if (raw.find("Developer Mode") != std::string::npos ||
      raw.find("developer mode") != std::string::npos) {
    return "the device refused on Developer Mode grounds: enable Settings > "
           "Privacy & Security > Developer Mode on the device and reconnect. "
           "(devicectl: " + trim(raw) + ")";
  }
  if (raw.find("passcode") != std::string::npos ||
      raw.find("locked") != std::string::npos) {
    return "the device is locked: unlock it and retry. (devicectl: " +
           trim(raw) + ")";
  }
  if (raw.find("trust") != std::string::npos ||
      raw.find("pairing") != std::string::npos) {
    return "the pairing was refused: accept the trust prompt on the device "
           "and retry. (devicectl: " + trim(raw) + ")";
  }
  // Unrecognised. The tool's own words, and no invented explanation on top.
  return trim(raw);
}

const char* to_string(Reachability r) {
  switch (r) {
    case Reachability::kReachable:   return "reachable";
    case Reachability::kNotFound:    return "not_found";
    case Reachability::kProbeFailed: return "probe_failed";
  }
  return "probe_failed";
}

/// Asks a simulator whether it is actually running anything.
///
/// `simctl list` reports a *state*, and a state is not an answer: a simulator
/// can sit in `Booted` while CoreSimulator is wedged, which read as usable
/// here and then failed on the first operation with an error about the
/// operation rather than about the simulator. Running `/usr/bin/true` inside
/// it is the cheapest question that requires the runtime to respond.
ReachabilityProbe probe_simulator(const std::string& udid,
                                  const discovery::ProviderOptions& opts) {
  ReachabilityProbe out;
  out.evidence = "xcrun simctl spawn <udid> /usr/bin/true";
  const auto started = std::chrono::steady_clock::now();
  proc::Options po;
  // A booted simulator answers in about 0.4 s and a shut-down one fails in
  // half that. Five seconds is generous and still bounded.
  po.timeout = std::chrono::milliseconds(5000);
  po.cancel = opts.cancel;
  const proc::Result r =
      proc::run({"xcrun", "simctl", "spawn", udid, "/usr/bin/true"}, po);
  out.took = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  if (!r.spawned) {
    out.detail = "could not run simctl: " + r.spawn_error;
    return out;
  }
  if (r.timed_out) {
    // Not "absent": a simulator that does not answer within five seconds is
    // exactly the wedged case, and calling it absent would lose the
    // distinction that matters.
    out.state = Reachability::kNotFound;
    out.detail = "the simulator did not run a trivial process within 5s: its "
                 "runtime is not responding, whatever `simctl list` reports "
                 "about its state";
    return out;
  }
  if (r.exit_code == 0) {
    out.state = Reachability::kReachable;
    out.detail = "the simulator ran a process on request";
    // A simulator has no lock screen to be behind.
    out.locked = false;
    return out;
  }
  const std::string combined = r.out + r.err;
  // "not running" is a different answer from "the probe did not settle it",
  // and only the first is a statement about the simulator. The wording here
  // is simctl's own, taken from what it actually prints: a shut-down
  // simulator gives SimError 405, "Process spawn via launchd failed because
  // device is not booted". Matched on the error number as well as the
  // sentence, since Apple can reword the sentence.
  if (combined.find("code=405") != std::string::npos ||
      combined.find("is not booted") != std::string::npos ||
      combined.find("Unable to boot") != std::string::npos ||
      combined.find("current state: Shutdown") != std::string::npos ||
      combined.find("Invalid device") != std::string::npos ||
      combined.find("Unable to lookup") != std::string::npos) {
    out.state = Reachability::kNotFound;
    out.detail = "the simulator is not running: " +
                 trim(r.err.empty() ? r.out : r.err);
    return out;
  }
  out.detail = "the probe failed: " +
               (r.err.empty() ? std::string("exit ") +
                                    std::to_string(r.exit_code)
                              : r.err);
  return out;
}

ReachabilityProbe probe_reachability(const std::string& device_id,
                                     const discovery::ProviderOptions& opts,
                                     model::DeviceForm form) {
  ReachabilityProbe out;
  if (!proc::is_safe_argument(device_id, /*reject_option_like=*/true)) {
    out.evidence = "no command was run";
    out.detail = "refusing to pass '" + device_id + "' as a device id";
    return out;
  }
  if (form == model::DeviceForm::kSimulator) {
    return probe_simulator(device_id, opts);
  }
  out.evidence = "xcrun devicectl device info lockState";
  const auto started = std::chrono::steady_clock::now();
  proc::Options po;
  // Short: an absent device fails in about 100 ms, and a present one answers
  // as quickly. A long budget here would only slow discovery down.
  po.timeout = std::chrono::milliseconds(6000);
  po.cancel = opts.cancel;
  const proc::Result r = proc::run(
      {"xcrun", "devicectl", "device", "info", "lockState", "--device",
       device_id, "--timeout", "5"}, po);
  out.took = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (!r.spawned) {
    out.detail = "could not run devicectl: " + r.spawn_error;
    return out;
  }
  if (r.timed_out) {
    // Not "not found": a timeout is the probe failing to answer, and calling
    // it absence would report a slow-to-respond device as gone.
    out.detail = "the probe did not finish within 6s, so whether the device "
                 "is there is unknown";
    return out;
  }
  const std::string combined = r.out + r.err;
  if (r.exit_code == 0) {
    out.state = Reachability::kReachable;
    // The device answered, which is the fact worth having. Its lock state is
    // in the answer, and a locked device cannot be driven for a capture.
    if (combined.find("passcodeRequired: true") != std::string::npos ||
        combined.find("\"isPasscodeRequired\" : true") != std::string::npos) {
      out.locked = true;
    } else if (combined.find("passcodeRequired: false") != std::string::npos ||
               combined.find("\"isPasscodeRequired\" : false") != std::string::npos) {
      out.locked = false;
    }
    out.detail = "the device answered a live query";
    return out;
  }
  // CoreDeviceError 1011 is "unable to locate a device matching the requested
  // device identifier" -- the device is not present. Matched on the error
  // number rather than the sentence, which Apple can reword.
  if (combined.find("error 1011") != std::string::npos ||
      combined.find("unable to locate a device") != std::string::npos) {
    out.state = Reachability::kNotFound;
    out.detail = "CoreDevice could not locate the device: it is not connected "
                 "by cable and not reachable on this network";
    return out;
  }
  // Error 1000 is a different thing from 1011: the identifier is not a
  // CoreDevice device at all. A simulator UDID lands here, and so does a
  // typo, and neither is a phone that has gone away.
  if (combined.find("error 1000") != std::string::npos ||
      combined.find("The specified device was not found") != std::string::npos) {
    out.detail = "'" + device_id +
                 "' is not a CoreDevice device: devicectl manages physical "
                 "devices, so a simulator UDID or a mistyped id gives this. "
                 "It is not a statement about any phone.";
    return out;
  }
  out.detail = "the probe failed: " +
               (r.err.empty() ? std::string("exit ") + std::to_string(r.exit_code)
                              : r.err);
  return out;
}

std::vector<std::string> explain_unusable_device(
    const model::DeviceRef& device, const discovery::ProviderOptions& opts,
    bool probe) {
  std::vector<std::string> out;
  if (device.platform != model::Platform::kIos) return out;

  if (device.form == model::DeviceForm::kSimulator) {
    // Two different simulator failures, and telling someone to boot one that
    // is already booted is worse than saying nothing.
    if (device.trust == model::TrustState::kUnknown) {
      out.push_back("this simulator reports Booted and did not answer a "
                    "liveness probe: `simctl` says it is running and its "
                    "runtime does not respond. `simctl list` reports a state "
                    "field, not an answer, which is why the state alone is "
                    "not trusted here.");
      out.push_back("shut it down and boot it again -- `xcrun simctl "
                    "shutdown " + device.device_id + "` then `mpi boot "
                    "--device " + device.device_id +
                    "`. If that does not settle it, CoreSimulator itself is "
                    "wedged: `xcrun simctl shutdown all` and, failing that, "
                    "quit the Simulator app.");
      if (probe) {
        // Ask again now: the earlier reading is from discovery, and a
        // simulator can recover between the two.
        const ReachabilityProbe again =
            probe_simulator(device.capture_id(), opts);
        if (again.state == Reachability::kReachable) {
          out.push_back("it answered just now, so it has recovered since "
                        "discovery ran -- refresh the device list.");
        }
      }
      return out;
    }
    out.push_back("this is a simulator and it is not booted. Start it from "
                  "the Devices tab, or `mpi boot --device " +
                  device.device_id + "`.");
    return out;
  }

  if (!device.last_seen_at.empty()) {
    // The single most useful fact, and the one that was being thrown away: a
    // device last seen in June is not a connection problem to debug, it is a
    // device that is somewhere else.
    out.push_back("devicectl last spoke to this device at " +
                  device.last_seen_at +
                  ". Everything else it reports about the device describes it "
                  "as it was then, not as it is now -- the listing is a cached "
                  "record, and it answers successfully for a device that is "
                  "not present.");
  }

  if (probe) {
    const ReachabilityProbe p = probe_reachability(device.capture_id(), opts);
    switch (p.state) {
      case Reachability::kNotFound:
        out.push_back("a live probe (" + p.evidence + ") could not find it: " +
                      p.detail + ". Connect it by USB, or put it on this "
                      "machine's network with Xcode's 'Connect via network' "
                      "enabled for it.");
        break;
      case Reachability::kReachable:
        // Worth saying loudly: the passive listing and the live probe
        // disagree, and the probe is the one that talked to the hardware.
        out.push_back("a live probe reached the device, which contradicts the "
                      "cached listing that marked it unusable. Re-run "
                      "discovery; if it still reads as unusable, that is a "
                      "bug in this tool rather than a problem with the "
                      "device.");
        if (p.locked.has_value() && *p.locked) {
          out.push_back("the device is locked. Unlock it: a locked device "
                        "will not run a capture.");
        }
        break;
      case Reachability::kProbeFailed:
        out.push_back("a live probe did not settle it: " + p.detail);
        break;
    }
  }

  // The remaining prerequisites, each stated only when it is the blocker.
  if (device.trust == model::TrustState::kUntrusted) {
    out.push_back("pairing or Developer Mode is the blocker rather than the "
                  "connection. On the device: trust this computer when asked, "
                  "and enable Settings > Privacy & Security > Developer Mode "
                  "(iOS 16 and later), then reconnect.");
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
      r.last_connection_date = str_at(*cp, "lastConnectionDate");
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
      r.last_connection_date = str_at(*cp, "lastConnectionDate");
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
      // Only a booted simulator can be inspected -- and `Booted` is a state
      // field, not an answer. CoreSimulator can hold a simulator in `Booted`
      // while its runtime is wedged, which reported as usable here and then
      // failed on the first operation with an error about the operation
      // rather than about the simulator.
      //
      // So a simulator claiming to be booted is asked to run a trivial
      // process. The cost is bounded by the number of *booted* simulators,
      // which is inherently small because each one holds real memory -- a
      // shut-down simulator is never probed, so a machine with twenty of
      // them pays nothing.
      // `Booted` here is what simctl *reports*. Whether the runtime answers
      // is a separate question, asked by the caller -- this function is a
      // pure parser and spawning a process from it would make its own tests
      // depend on a live simulator.
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

  {
    // A host prerequisite, not a device one, and the reason a capture can
    // come back with an empty table on a perfectly good device: Instruments
    // samples through the unified log store, and a process without Full Disk
    // Access cannot open it. The capture path learned to say this; preflight
    // did not, so someone would see a clean preflight and then hit
    // permission_denied on the capture -- which is the one thing preflight
    // exists to prevent.
    const LogStoreAccess access = probe_log_store_access();
    auto c = make_cap("ios.capture.log_store",
                      "Unified log store access (Instruments sampling)",
                      access.readable ? model::CapabilityStatus::kAvailable
                                      : model::CapabilityStatus::kPermissionDenied,
                      "log");
    c.prerequisites.push_back(
        "Full Disk Access for whatever runs this tool, or running the capture "
        "from Xcode");
    c.evidence = access.readable
                     ? "`log show --last 1s` opened the local log store"
                     : "`log show --last 1s` was refused: " + access.detail;
    if (!access.readable) {
      c.limitations.push_back(
          "xctrace reports this as \"the log archive is corrupt or incomplete "
          "and cannot be read\", which describes a damaged machine and is "
          "usually this permission. Every recording here came back with a "
          "time-profile table that had a schema and no rows.");
      c.recovery_action =
          "System Settings > Privacy & Security > Full Disk Access, and add "
          "the terminal or app that runs this tool; or run the capture from "
          "Xcode, which already has it";
    }
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
        // Same reason as the refusal in list_apps: this is a reading from a
        // cached record, and a capability's evidence without its date is an
        // observation presented as a standing fact.
        if (!readiness->last_connection_date.empty()) {
          c.evidence += " (devicectl's reading as of " +
                        readiness->last_connection_date + ")";
        }
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
    // Live capture is now two different answers on iOS, and reporting only
    // the physical-device one contradicted the tool itself: a booted
    // simulator streams through SimulatorHostCollector, while this said live
    // capture was unverified. Preflight and the Live tab telling an operator
    // opposite things is the same defect as the old "the collector is not
    // wired" message, which was also false.
    auto c = make_cap("ios.capture.live_recording",
                      "Live capture (physical device, and simulator)",
                      model::CapabilityStatus::kUnknown, "xctrace");
    c.provider_version = xt_version;
    c.prerequisites.push_back("a reachable physical device, OR a booted "
                              "simulator for the host-process collector");
    c.prerequisites.push_back("an attachable, developer-signed app");

    if (sim != nullptr) {
      // Measured, not assumed: the host-process collector was driven against
      // a booted simulator and returned CPU time, utilisation, memory
      // footprint and per-thread times.
      c.status = model::CapabilityStatus::kLimited;
      c.tested = model::TestedState::kVerifiedOnSimulatorOrEmulator;
      c.evidence =
          "a booted simulator is present and live capture works there "
          "without Instruments: the app is an ordinary host process, so CPU "
          "time and memory footprint are read through libproc. xctrace is "
          "not involved.";
      c.limitations.push_back(
          "on a simulator this gives CPU *time* and memory, not frames and "
          "not stacks. The frame part is a platform limit -- no command-line "
          "frame source exists for a simulator. The stack part is not: "
          "`/usr/bin/sample` profiles a simulator app and returns a "
          "symbolised call graph, and this build does not ingest it yet, so "
          "attribution is a gap here rather than an impossibility.");
      c.limitations.push_back(
          "simulator timings are not device timings, and an app running "
          "translated under Rosetta is not comparable to a native build "
          "either -- the capture records which");
    } else {
      c.evidence =
          xt_version.empty()
              ? "xctrace is not available on this host"
              : xt_version +
                    " is installed; no booted simulator is present, and live "
                    "capture has NOT been exercised against a physical "
                    "device in this environment";
    }

    // Always stated, whether or not a simulator is present. A usable
    // simulator does not make the device path verified, and dropping the
    // note when one happens to be booted would let a green simulator answer
    // stand in for hardware that has never been tested.
    c.limitations.push_back(
        "UNVERIFIED on a physical device: no physical iOS device has been "
        "reachable, so that path is implemented and not demonstrated. "
        "Instruments cannot record a *simulator* target at all on this host "
        "-- it accepts the target and never starts recording -- so a working "
        "simulator answer does not transfer to hardware.");
    c.recovery_action =
        sim != nullptr
            ? "for a device, connect a trusted physical iPhone or iPad and "
              "re-run `mpi preflight`; the simulator path is already usable"
            : "boot a simulator, or connect a trusted physical iPhone or "
              "iPad, and run `mpi preflight` to convert this from unknown to "
              "a measured result";
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
    errors.push_back("devicectl device listing failed: " +
                     describe_devicectl_failure(err));
  }

  if (include_simulators_) {
    proc::Options po;
    po.timeout = std::chrono::milliseconds(opts.command_timeout_ms);
    po.cancel = opts.cancel;
    const auto r = proc::run({"xcrun", "simctl", "list", "devices", "--json"}, po);
    if (r.ok()) {
      json::ParseError perr;
      if (auto doc = json::parse(r.out, json::Limits{}, &perr)) {
        for (auto& d : parse_simctl_devices(*doc, "")) {
          // simctl reported `Booted`, which is a state field and not an
          // answer: CoreSimulator can hold a simulator in `Booted` while its
          // runtime is wedged, and that read as usable here and then failed
          // on the first operation -- with an error about the operation
          // rather than about the simulator.
          //
          // Only a simulator claiming to be booted is asked, so the cost is
          // bounded by how many are actually running. That is inherently
          // small, because each one holds real memory; a machine with twenty
          // shut-down simulators pays nothing. Measured: 0.37 s for one that
          // answers.
          if (d.trust == model::TrustState::kAuthorized) {
            const ReachabilityProbe live =
                probe_simulator(d.device_id, opts);
            if (live.state != Reachability::kReachable) {
              // kUnknown, not kOffline: "booted and not answering" is
              // neither "shut down" nor "usable", and what to do about it
              // differs from both. Either way it is not usable, which is the
              // half that matters.
              d.trust = model::TrustState::kUnknown;
              errors.push_back(
                  "simulator '" + d.display_name + "' (" + d.device_id +
                  ") reports Booted and did not answer a liveness probe: " +
                  live.detail);
            }
          }
          out.push_back(std::move(d));
        }
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
    errors.push_back(ddi_refusal_text(*readiness));
    enumeration_failed = true;
    return out;
  }

  auto apps_doc = run_devicectl_json(
      {"device", "info", "apps", "--device", device.device_id}, opts, &err);
  if (!apps_doc) {
    errors.push_back("`devicectl device info apps` failed: " +
                     describe_devicectl_failure(err));
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
          "unknown: " + describe_devicectl_failure(perr_msg));
    }
  }

  // Whether the attribution rule matched anything at all on this device.
  //
  // It is the difference between "this app is not running" and "the rule does
  // not fit this device". A rule that matches nothing anywhere has not
  // observed an idle device; it has failed, and saying so is the whole point.
  bool attribution_ever_worked = false;
  if (processes_ok) {
    for (const auto& app : installed) {
      for (const auto& p : processes) {
        if (executable_path_names_bundle(p.executable_path, app.bundle_id)) {
          attribution_ever_worked = true;
          break;
        }
      }
      if (attribution_ever_worked) break;
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
        if (!executable_path_names_bundle(p.executable_path, app.bundle_id)) {
          continue;
        }
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
      if (!e.processes.empty()) {
        e.runtime_state = model::RuntimeState::kRunning;
        e.notes.push_back(
            "running; iOS exposes no general foreground/suspended signal");
      } else if (attribution_ever_worked) {
        // The rule has matched something on this device, so it works here,
        // and nothing matching this app means this app is not running.
        e.runtime_state = model::RuntimeState::kNotRunning;
      } else {
        // Nothing matched *any* installed app while processes were listed.
        // That is not evidence the device is idle: it is equally consistent
        // with the attribution rule not fitting this device's path shape,
        // which is exactly what happened when the rule required the bundle
        // id as a whole path segment -- real container paths carry it as a
        // segment prefix, and a physical device's do not carry it at all.
        //
        // Reporting not_running here would have told someone to start an app
        // that was already running.
        e.runtime_state = model::RuntimeState::kUnknown;
        e.notes.push_back(
            "runtime state is unknown: " + std::to_string(processes.size()) +
            " process(es) were listed and none could be attributed to any "
            "installed app. Attribution relies on the executable path naming "
            "the bundle, which devicectl does not guarantee, so this is not "
            "evidence that the app is not running");
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
