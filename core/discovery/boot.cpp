#include "core/discovery/boot.hpp"

#include <set>
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

/// Where `adb` is, on a machine that has not put the SDK on PATH.
///
/// PATH is the obvious answer and it is not enough on its own. An app
/// launched from the Dock does not inherit a shell's environment, and on
/// this host a Finder launch gets `/usr/bin:/bin:/usr/sbin:/sbin` and
/// nothing else -- not even what `launchctl setenv PATH` was set to, which
/// was measured rather than assumed. So a tool that resolves in a terminal
/// is simply absent in DevX, and the failure read as a broken Android SDK:
/// "adb: No such file or directory", beside an emulator list that had just
/// been produced successfully, because the emulator already resolved
/// through the SDK location below and adb did not.
///
/// Same chain as `resolve_emulator_path`, for the same reason and in the
/// same order: an explicit setting, then the environment variables that
/// name a specific SDK, then the default install location, then PATH.
std::string resolve_adb_path(const std::string& configured) {
  if (!configured.empty() && configured != "adb") return configured;
  for (const char* key : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
    const auto root = env(key);
    if (root.empty()) continue;
    const auto candidate = root + "/platform-tools/adb";
    if (file_exists(candidate)) return candidate;
  }
  const auto home = env("HOME");
  if (!home.empty()) {
    const auto candidate = home + "/Library/Android/sdk/platform-tools/adb";
    if (file_exists(candidate)) return candidate;
  }
  // Nothing found: keep the bare name so PATH still gets its chance, and so
  // a failure quotes the name the reader recognises.
  return configured.empty() ? std::string("adb") : configured;
}


// Every serial adb can see, with its state.
//
// The state is kept rather than filtered on, because "a new serial appeared
// and is `offline`" is a much more useful answer than "no new device": it
// says the emulator started and has not finished coming up, which is what a
// slow cold boot looks like. Measured on this host: a second AVD produced
// `emulator-5556 offline` and stayed there past a 150 s budget.
std::vector<std::pair<std::string, std::string>> adb_devices_with_state(
    const BootOptions& opts,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(15000),
    std::string* tool_error = nullptr) {
  proc::Options po;
  po.timeout = timeout;
  po.cancel = opts.cancel;
  const auto r = proc::run({resolve_adb_path(opts.adb_path), "devices"}, po);
  std::vector<std::pair<std::string, std::string>> out;
  if (tool_error != nullptr) tool_error->clear();
  if (!r.ok()) {
    // An empty list and a failed adb used to be the same answer, so an adb
    // that stopped answering was reported as an emulator that never
    // appeared. They are different claims and only one of them is about the
    // emulator.
    if (tool_error != nullptr) {
      *tool_error = !r.spawned
                        ? r.spawn_error
                        : (r.timed_out ? "`adb devices` did not answer within "
                                             + std::to_string(timeout.count())
                                             + " ms"
                                       : trim(r.err));
      if (tool_error->empty()) *tool_error = "`adb devices` failed";
    }
    return out;
  }
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

/// The AVD a running emulator serial is an instance of, or empty.
///
/// `adb devices` lists serials and never AVD names, and this code used to
/// conclude from that that the question was unanswerable, leaving every
/// Android target marked not-running. It is answerable -- the emulator
/// console answers it per serial. Measured on this host:
///
///     $ adb -s emulator-5554 emu avd name
///     Pixel_9_Pro
///     OK
///
/// Getting it wrong was not cosmetic. `boot()` already has a no-op path for
/// a target that is running; with the flag always false, Start spawned a
/// *second* emulator for an AVD already running, which does not fail
/// quickly -- it hangs until the ready budget expires.
///
/// One call per connected serial, and emulators are heavy enough that there
/// are never many. A serial that is not an emulator answers with an error,
/// treated here as "no name" rather than a failure worth reporting: the only
/// thing riding on it is whether a target can be marked running.
std::string avd_name_of_serial(const std::string& serial,
                               const BootOptions& opts) {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(5000);
  po.cancel = opts.cancel;
  const auto r = proc::run(
      {resolve_adb_path(opts.adb_path), "-s", serial, "emu", "avd", "name"},
      po);
  if (!r.ok()) return {};
  // The console replies with the name, then `OK` on its own line.
  for (const auto& line : lines_of(r.out)) {
    const auto name = trim(line);
    if (name.empty() || name == "OK") continue;
    return name;
  }
  return {};
}

std::vector<std::string> adb_serials(const BootOptions& opts) {
  std::vector<std::string> out;
  for (const auto& [serial, state] : adb_devices_with_state(opts)) {
    if (state == "device") out.push_back(serial);
  }
  return out;
}

bool android_boot_completed(
    const std::string& serial, const BootOptions& opts,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
  proc::Options po;
  po.timeout = timeout;
  po.cancel = opts.cancel;
  const auto r = proc::run(
      {resolve_adb_path(opts.adb_path), "-s", serial, "shell", "getprop",
       "sys.boot_completed"},
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
      // Which AVDs those serials are instances of, so one already running
      // is reported as running instead of started a second time.
      std::set<std::string> running_avds;
      for (const auto& serial : running) {
        const auto avd = avd_name_of_serial(serial, opts);
        if (!avd.empty()) running_avds.insert(avd);
      }
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
// Answered per serial by the emulator console rather than guessed
          // from adb's listing, which carries no AVD name. Leaving this
          // false let Start launch a second emulator for an AVD already
          // running, and that hangs rather than failing.
          t.already_running = running_avds.count(name) > 0;
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

/// How long to sleep between polls without outliving the budget.
///
/// The sleep happens after the budget check, so an unclamped one could push
/// the loop past the deadline by a whole poll interval -- small next to a
/// 180 s budget and not small next to a 3 s one, which is what a test that
/// pins the bound would trip over.
std::chrono::milliseconds poll_pause(std::chrono::milliseconds elapsed,
                                     std::chrono::milliseconds ready_timeout,
                                     std::chrono::milliseconds interval) {
  if (elapsed >= ready_timeout) return std::chrono::milliseconds(0);
  const auto remaining = ready_timeout - elapsed;
  return remaining < interval ? remaining : interval;
}

std::optional<std::chrono::milliseconds> poll_budget(
    std::chrono::milliseconds elapsed,
    std::chrono::milliseconds ready_timeout,
    std::chrono::milliseconds per_call_cap,
    std::chrono::milliseconds floor) {
  if (elapsed >= ready_timeout) return std::nullopt;
  const auto remaining = ready_timeout - elapsed;
  if (remaining <= floor) return std::nullopt;
  return remaining < per_call_cap ? remaining : per_call_cap;
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
    //
    // How many consecutive polls simctl itself failed to answer. Tracked
    // because "the device is still booting" and "the tooling stopped
    // answering" need different things done about them, and the second one
    // used to be reported as the first.
    int unanswered = 0;
    // Whether simctl answered even once since the boot was asked for. When
    // the budget runs out before three polls fit, consecutive misses alone
    // would let a tool that never answered be reported as a simulator that
    // did not boot.
    bool answered = false;
    std::string last_tool_error;
    bool tool_timed_out = false;
    for (;;) {
      if (opts.cancel.cancelled()) {
        res.notes.push_back("the wait was cancelled; the simulator may still "
                            "be booting");
        break;
      }
      // Whatever is left of the budget, capped. A fixed per-call timeout let
      // a poll starting just inside the budget run a minute past it.
      const auto budget = poll_budget(elapsed(), opts.ready_timeout,
                                      std::chrono::milliseconds(15000),
                                      std::chrono::milliseconds(250));
      if (!budget.has_value()) break;
      po.timeout = *budget;
      const auto list = proc::run(
          {"/usr/bin/xcrun", "simctl", "list", "devices", "--json"}, po);
      if (!list.ok()) {
        unanswered++;
        if (list.timed_out) tool_timed_out = true;
        last_tool_error = list.spawned
                              ? (list.timed_out
                                     ? "it did not answer within " +
                                           std::to_string(po.timeout.count()) +
                                           " ms"
                                     : trim(list.err))
                              : list.spawn_error;
      } else {
        unanswered = 0;
        answered = true;
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
      // Three in a row is not a blip. Continuing would spend the rest of the
      // budget re-asking a service that is not answering, and then blame the
      // simulator for not booting.
      if (unanswered >= 3) break;
      std::this_thread::sleep_for(
          poll_pause(elapsed(), opts.ready_timeout, opts.poll_interval));
    }
    res.waited = elapsed();
    if (!res.ready) {
      if (unanswered >= 3 || (unanswered > 0 && !answered)) {
        // A provider failure, which is a different claim from a device that
        // did not come up -- and the one the operator can act on.
        res.error = "simctl stopped answering while waiting for the "
                    "simulator: " + last_tool_error;
        res.notes.push_back(
            std::string("this is a failure of the simulator tooling, not an "
                        "answer about the device: nothing here says whether ") +
            target.identifier + " booted." +
            (tool_timed_out
                 ? " A hung simctl usually means CoreSimulatorService is "
                   "wedged; `sudo pkill -f CoreSimulatorService` clears it, "
                   "and it comes back on its own."
                 : ""));
      } else {
        res.notes.push_back(
            "the simulator was asked to boot and did not reach the `Booted` "
            "state within the budget. It may still come up; nothing here "
            "claims it did");
      }
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

  if (target.already_running) {
    // The same no-op the iOS branch takes, which Android never needed while
    // `already_running` was hardcoded false. Starting a second emulator for
    // an AVD already running does not fail fast: it spawns, the new instance
    // cannot claim the AVD, and the wait runs to the end of the ready budget
    // -- which is what "sometimes it will not start" looked like.
    //
    // The serial is looked up rather than assumed: a BootTarget's identifier
    // is an AVD name on Android, never a device id, and the caller needs the
    // id to record against.
    res.started = true;
    res.ready = true;
    res.was_already_running = true;
    for (const auto& serial : adb_serials(opts)) {
      if (avd_name_of_serial(serial, opts) == target.identifier) {
        res.device_id = serial;
        break;
      }
    }
    res.notes.push_back(
        res.device_id.empty()
            ? "this AVD is already running; nothing was started. Its serial "
              "could not be confirmed, so no device id is claimed here."
            : "this AVD is already running as " + res.device_id +
                  "; nothing was started");
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
  //
  // Bounded against the same budget as the polls. It used to take its own
  // fixed 15 s outside the budget entirely: with a hung adb that alone spent
  // 15 s of a 3 s budget, and the loop then had nothing left to poll with.
  std::string baseline_error;
  std::vector<std::string> before;
  {
    const auto baseline_budget = poll_budget(
        elapsed(), opts.ready_timeout, std::chrono::milliseconds(15000),
        std::chrono::milliseconds(250));
    if (!baseline_budget.has_value()) {
      res.error = "no budget left to look at `adb devices` before starting "
                  "the emulator, so nothing was started";
      return res;
    }
    for (const auto& [serial, state] : adb_devices_with_state(
             opts, *baseline_budget, &baseline_error)) {
      static_cast<void>(state);
      before.push_back(serial);
    }
  }
  if (!baseline_error.empty()) {
    // Without a baseline there is no way to tell which serial this boot
    // produced, and an empty one is the dangerous answer rather than a
    // harmless one: every emulator already running would look like it had
    // just appeared, and this would report someone else's device as the one
    // it started.
    //
    // So it does not start. Leaving an emulator running that cannot be
    // identified is worse than not starting one, and the reason is
    // actionable.
    res.error = "adb did not answer before the emulator was started, so the "
                "serials already present are unknown: " + baseline_error;
    res.notes.push_back(
        "nothing was started. Without that list a new serial cannot be told "
        "from one that was already there, and this would have reported an "
        "emulator it did not start. `adb kill-server` and a retry is the "
        "usual fix");
    return res;
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
  int unanswered = 0;
  // Whether adb answered even once after the emulator was started; see the
  // simulator path for why consecutive misses alone are not enough.
  bool answered = false;
  std::string last_tool_error;
  for (;;) {
    if (opts.cancel.cancelled()) {
      res.notes.push_back(
          "the wait was cancelled; the emulator may still be booting");
      break;
    }
    // Whatever is left, capped -- and split between the two calls a poll
    // makes, so one poll cannot spend the whole remaining budget and leave
    // nothing for the check that actually decides readiness.
    const auto budget = poll_budget(elapsed(), opts.ready_timeout,
                                    std::chrono::milliseconds(15000),
                                    std::chrono::milliseconds(250));
    if (!budget.has_value()) break;
    const auto half = std::chrono::milliseconds(budget->count() / 2 + 1);

    // The serial that appeared. Taken as a difference rather than assumed to
    // be `emulator-5554`: the port depends on what was already running, and
    // booting a second AVD lands on 5556.
    std::string appeared;
    std::string appeared_state;
    std::string tool_error;
    for (const auto& [serial, state] :
         adb_devices_with_state(opts, half, &tool_error)) {
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
    if (!tool_error.empty()) {
      // adb did not answer. That is not "no new device": it is no answer.
      unanswered++;
      last_tool_error = tool_error;
      if (unanswered >= 3) break;
      std::this_thread::sleep_for(
          poll_pause(elapsed(), opts.ready_timeout, opts.poll_interval));
      continue;
    }
    unanswered = 0;
    answered = true;
    if (!appeared.empty()) {
      last_seen_serial = appeared;
      last_seen_state = appeared_state;
      if (appeared_state == "device" &&
          android_boot_completed(appeared, opts, half)) {
        res.ready = true;
        res.device_id = appeared;
        break;
      }
    }
    std::this_thread::sleep_for(
        poll_pause(elapsed(), opts.ready_timeout, opts.poll_interval));
  }
  res.waited = elapsed();
  if (!res.ready) {
    if (unanswered >= 3 || (unanswered > 0 && !answered)) {
      res.error = "adb stopped answering while waiting for the emulator: " +
                  last_tool_error;
      res.notes.push_back(
          "this is a failure of the Android tooling, not an answer about the "
          "emulator: nothing here says whether " + target.identifier +
          " came up. `adb kill-server` and a retry is the usual fix");
    } else if (!last_seen_serial.empty()) {
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
