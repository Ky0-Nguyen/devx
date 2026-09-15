#include "core/discovery/boot.hpp"

#include <cstdlib>
#include <thread>

#include "core/util/json.hpp"

namespace mpi::discovery {
namespace {

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::string line;
  for (const char c : text) {
    if (c == '\n') {
      out.push_back(trim(line));
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) out.push_back(trim(line));
  return out;
}

std::string env(const char* key) {
  const char* v = std::getenv(key);
  return v == nullptr ? std::string() : std::string(v);
}

bool file_exists(const std::string& path) {
  if (path.empty()) return false;
  proc::Options po;
  po.timeout = std::chrono::milliseconds(3000);
  const auto r = proc::run({"/bin/test", "-x", path}, po);
  return r.ok();
}

// Every serial adb can see, with its state.
//
// The state is kept rather than filtered on, because "a new serial appeared
// and is `offline`" is a much more useful answer than "no new device": it
// says the emulator started and has not finished coming up, which is what a
// slow cold boot looks like. Measured on this host: a second AVD produced
// `emulator-5556 offline` and stayed there past a 150 s budget.
std::vector<std::pair<std::string, std::string>> adb_devices_with_state(
    const BootOptions& opts) {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(15000);
  po.cancel = opts.cancel;
  const auto r = proc::run({opts.adb_path, "devices"}, po);
  std::vector<std::pair<std::string, std::string>> out;
  if (!r.ok()) return out;
  for (const auto& line : lines_of(r.out)) {
    if (line.empty() || line.rfind("List of devices", 0) == 0) continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const std::string serial = trim(line.substr(0, tab));
    const std::string state = trim(line.substr(tab + 1));
    if (!serial.empty()) out.emplace_back(serial, state);
  }
  return out;
}

std::vector<std::string> adb_serials(const BootOptions& opts) {
  std::vector<std::string> out;
  for (const auto& [serial, state] : adb_devices_with_state(opts)) {
    if (state == "device") out.push_back(serial);
  }
  return out;
}

bool android_boot_completed(const std::string& serial, const BootOptions& opts) {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(10000);
  po.cancel = opts.cancel;
  const auto r = proc::run(
      {opts.adb_path, "-s", serial, "shell", "getprop", "sys.boot_completed"},
      po);
  return r.ok() && trim(r.out) == "1";
}

}  // namespace

json::Value BootTarget::to_json() const {
  json::Value o = json::Value::object();
  o.set("platform", json::Value::string(model::to_string(platform)));
  o.set("identifier", json::Value::string(identifier));
  o.set("display_name", json::Value::string(display_name));
  o.set("os_version", json::Value::string(os_version));
  o.set("already_running", json::Value::boolean(already_running));
  o.set("provider", json::Value::string(provider));
  return o;
}

json::Value BootTargets::to_json() const {
  json::Value o = json::Value::object();
  json::Value arr = json::Value::array();
  for (const auto& t : targets) arr.push_back(t.to_json());
  o.set("boot_targets", std::move(arr));
  json::Value errs = json::Value::array();
  for (const auto& e : errors) errs.push_back(json::Value::string(e));
  o.set("errors", std::move(errs));
  return o;
}

json::Value BootResult::to_json() const {
  json::Value o = json::Value::object();
  o.set("started", json::Value::boolean(started));
  o.set("ready", json::Value::boolean(ready));
  o.set("was_already_running", json::Value::boolean(was_already_running));
  // Null, not "": an unconfirmed device id is not an empty one.
  o.set("device_id", device_id.empty() ? json::Value::null()
                                       : json::Value::string(device_id));
  o.set("waited_ms", json::Value::integer(waited.count()));
  if (!error.empty()) o.set("error", json::Value::string(error));
  json::Value n = json::Value::array();
  for (const auto& note : notes) n.push_back(json::Value::string(note));
  o.set("notes", std::move(n));
  return o;
}

std::string resolve_emulator_path(const BootOptions& opts) {
  if (!opts.emulator_path.empty()) return opts.emulator_path;
  for (const char* key : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
    const auto root = env(key);
    if (root.empty()) continue;
    const auto candidate = root + "/emulator/emulator";
    if (file_exists(candidate)) return candidate;
  }
  // The default SDK location, which is where it is on a machine that has
  // never set the environment variables.
  const auto home = env("HOME");
  if (!home.empty()) {
    const auto candidate = home + "/Library/Android/sdk/emulator/emulator";
    if (file_exists(candidate)) return candidate;
  }
  proc::Options po;
  po.timeout = std::chrono::milliseconds(5000);
  const auto which = proc::run({"/usr/bin/which", "emulator"}, po);
  if (which.ok()) {
    const auto path = trim(which.out);
    if (!path.empty()) return path;
  }
  return {};
}

BootTargets list_boot_targets(const BootOptions& opts) {
  BootTargets out;

  // ---- Android AVDs ------------------------------------------------------
  const auto emulator = resolve_emulator_path(opts);
  if (emulator.empty()) {
    out.errors.push_back(
        "the Android `emulator` binary was not found (looked at "
        "ANDROID_HOME, ANDROID_SDK_ROOT, ~/Library/Android/sdk and PATH), so "
        "no AVD can be listed. That is not the same as having no AVDs");
  } else {
    proc::Options po;
    po.timeout = std::chrono::milliseconds(20000);
    po.cancel = opts.cancel;
    const auto r = proc::run({emulator, "-list-avds"}, po);
    if (!r.ok()) {
      out.errors.push_back("`emulator -list-avds` failed: " +
                           (r.spawned ? trim(r.err) : r.spawn_error));
    } else {
      const auto running = adb_serials(opts);
      for (const auto& name : lines_of(r.out)) {
        if (name.empty()) continue;
        // The listing prints warnings on stdout on some SDKs; anything with a
        // space is not an AVD name.
        if (name.find(' ') != std::string::npos) continue;
        BootTarget t;
        t.platform = model::Platform::kAndroid;
        t.identifier = name;
        t.display_name = name;
        t.provider = "emulator -list-avds";
        // An AVD's name is not in adb's listing, so "already running" cannot
        // be answered per AVD from adb alone. It is left false and the note
        // on the result says so rather than guessing from the serial.
        t.already_running = false;
        out.targets.push_back(std::move(t));
      }
      if (!running.empty()) {
        out.errors.push_back(
            std::to_string(running.size()) +
            " emulator/device serial(s) are already connected; adb does not "
            "say which AVD each one is, so no target below is marked as "
            "already running");
      }
    }
  }

  // ---- iOS simulators ----------------------------------------------------
  {
    proc::Options po;
    po.timeout = std::chrono::milliseconds(30000);
    po.cancel = opts.cancel;
    const auto r = proc::run(
        {"/usr/bin/xcrun", "simctl", "list", "devices", "available", "--json"},
        po);
    if (!r.ok()) {
      out.errors.push_back("`simctl list devices` failed: " +
                           (r.spawned ? trim(r.err) : r.spawn_error));
    } else {
      json::ParseError perr;
      auto parsed = json::parse(r.out, json::Limits{}, &perr);
      if (!parsed) {
        out.errors.push_back("simctl's device list could not be parsed: " +
                             perr.message);
      } else if (const json::Value* devices = parsed->find("devices")) {
        for (const auto& [runtime, list] : devices->members()) {
          if (!list.is_array()) continue;
          for (const auto& d : list.items()) {
            const json::Value* udid = d.find("udid");
            const json::Value* name = d.find("name");
            const json::Value* state = d.find("state");
            if (udid == nullptr || !udid->is_string()) continue;
            BootTarget t;
            t.platform = model::Platform::kIos;
            t.identifier = udid->as_string();
            t.display_name =
                name != nullptr && name->is_string() ? name->as_string() : "";
            // The runtime key is like "com.apple.CoreSimulator.SimRuntime.iOS-26-5".
            const auto at = runtime.rfind("iOS-");
            if (at != std::string::npos) {
              t.os_version = runtime.substr(at + 4);
              for (char& c : t.os_version) {
                if (c == '-') c = '.';
              }
            }
            t.already_running = state != nullptr && state->is_string() &&
                                state->as_string() == "Booted";
            t.provider = "simctl";
            out.targets.push_back(std::move(t));
          }
        }
      }
    }
  }
  return out;
}

BootResult boot(const BootTarget& target, const BootOptions& opts) {
  BootResult res;
  if (target.identifier.empty()) {
    res.error = "no target identifier was given";
    return res;
  }
  if (!proc::is_safe_argument(target.identifier, /*reject_option_like=*/true)) {
    res.error = "refusing to boot '" + target.identifier +
                "': it would be read as a command-line option";
    return res;
  }

  const auto started_at = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
  };

  if (target.platform == model::Platform::kIos) {
    proc::Options po;
    po.timeout = std::chrono::milliseconds(60000);
    po.cancel = opts.cancel;
    if (target.already_running) {
      res.started = true;
      res.ready = true;
      res.was_already_running = true;
      res.device_id = target.identifier;
      res.notes.push_back("this simulator was already booted; nothing was "
                          "started");
      return res;
    }
    const auto r = proc::run(
        {"/usr/bin/xcrun", "simctl", "boot", target.identifier}, po);
    if (!r.ok()) {
      res.error = "`simctl boot` failed: " +
                  (r.spawned ? trim(r.err) : r.spawn_error);
      return res;
    }
    res.started = true;
    // simctl returns before the runtime is usable, so the state is polled.
    while (elapsed() < opts.ready_timeout) {
      if (opts.cancel.cancelled()) {
        res.notes.push_back("the wait was cancelled; the simulator may still "
                            "be booting");
        break;
      }
      const auto list = proc::run(
          {"/usr/bin/xcrun", "simctl", "list", "devices", "--json"}, po);
      if (list.ok() && list.out.find(target.identifier) != std::string::npos) {
        json::ParseError perr;
        auto parsed = json::parse(list.out, json::Limits{}, &perr);
        bool booted = false;
        if (parsed) {
          if (const json::Value* devices = parsed->find("devices")) {
            for (const auto& [runtime, arr] : devices->members()) {
              static_cast<void>(runtime);
              if (!arr.is_array()) continue;
              for (const auto& d : arr.items()) {
                const json::Value* udid = d.find("udid");
                const json::Value* state = d.find("state");
                if (udid == nullptr || !udid->is_string()) continue;
                if (udid->as_string() != target.identifier) continue;
                booted = state != nullptr && state->is_string() &&
                         state->as_string() == "Booted";
              }
            }
          }
        }
        if (booted) {
          res.ready = true;
          res.device_id = target.identifier;
          break;
        }
      }
      std::this_thread::sleep_for(opts.poll_interval);
    }
    res.waited = elapsed();
    if (!res.ready) {
      res.notes.push_back(
          "the simulator was asked to boot and did not reach the `Booted` "
          "state within the budget. It may still come up; nothing here "
          "claims it did");
    } else {
      // The runtime is up; the Simulator UI is a separate application and is
      // not required for capture, so it is only mentioned.
      res.notes.push_back(
          "the runtime is booted. The Simulator window is a separate app -- "
          "`open -a Simulator` -- and is not needed to capture");
    }
    return res;
  }

  if (target.platform != model::Platform::kAndroid) {
    res.error = "no way to boot a target on platform '" +
                std::string(model::to_string(target.platform)) + "'";
    return res;
  }

  const auto emulator = resolve_emulator_path(opts);
  if (emulator.empty()) {
    res.error =
        "the Android `emulator` binary was not found, so no AVD can be "
        "started";
    return res;
  }

  // Every serial present beforehand, whatever its state, so a serial that
  // was already offline is not mistaken for one this boot produced.
  std::vector<std::string> before;
  for (const auto& [serial, state] : adb_devices_with_state(opts)) {
    static_cast<void>(state);
    before.push_back(serial);
  }
  // Detached: the emulator runs for as long as the device is up, which is far
  // longer than this call. Its output goes nowhere rather than filling a
  // buffer nobody reads.
  const auto spawn = proc::spawn_detached(
      {emulator, "-avd", target.identifier, "-no-snapshot-save"});
  if (!spawn.spawned) {
    res.error = "could not start the emulator: " + spawn.error;
    return res;
  }
  res.started = true;

  std::string last_seen_serial;
  std::string last_seen_state;
  while (elapsed() < opts.ready_timeout) {
    if (opts.cancel.cancelled()) {
      res.notes.push_back(
          "the wait was cancelled; the emulator may still be booting");
      break;
    }
    // The serial that appeared. Taken as a difference rather than assumed to
    // be `emulator-5554`: the port depends on what was already running, and
    // booting a second AVD lands on 5556.
    std::string appeared;
    std::string appeared_state;
    for (const auto& [serial, state] : adb_devices_with_state(opts)) {
      bool was_there = false;
      for (const auto& b : before) {
        if (b == serial) was_there = true;
      }
      if (!was_there) {
        appeared = serial;
        appeared_state = state;
        break;
      }
    }
    if (!appeared.empty()) {
      last_seen_serial = appeared;
      last_seen_state = appeared_state;
      if (appeared_state == "device" && android_boot_completed(appeared, opts)) {
        res.ready = true;
        res.device_id = appeared;
        break;
      }
    }
    std::this_thread::sleep_for(opts.poll_interval);
  }
  res.waited = elapsed();
  if (!res.ready) {
    if (!last_seen_serial.empty()) {
      // Far more useful than "no new device": the emulator did start, and
      // this says how far it got.
      res.notes.push_back(
          "the emulator started and " + last_seen_serial +
          " appeared, but it never reported sys.boot_completed within the "
          "budget -- adb last saw it as '" + last_seen_state +
          "'. It may still be coming up; nothing here claims it is ready, and "
          "a capture against it now would measure the boot");
    } else {
      res.notes.push_back(
          "the emulator was started and no new serial appeared in `adb "
          "devices` within the budget, so nothing can be said about whether "
          "it is coming up");
    }
  }
  return res;
}

}  // namespace mpi::discovery
