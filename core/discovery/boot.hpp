// Starting a simulator or emulator, and waiting until it can actually answer.
//
// Discovery lists what is *there*. This starts something that is not, which is
// a different kind of operation and is kept separate on purpose: it changes
// the machine's state rather than observing it.
//
// Three things make this honest rather than a convenience wrapper:
//
//   * **An AVD name is not a device id.** `emulator -avd Pixel_9_Pro` produces
//     a device that adb calls `emulator-5554`, and nothing guarantees which
//     port it lands on. So a boot reports the device id it *observed*
//     afterwards, and says it could not confirm one rather than guessing.
//   * **"Started" is not "ready".** The emulator process exists long before
//     the device answers; Android's `sys.boot_completed` and simctl's
//     `Booted` state are the signals, and they are waited for rather than
//     assumed. A capture against a half-booted device measures the boot.
//   * **A booted emulator is still an emulator.** Nothing here changes the
//     device form, and every measurement taken on it stays labelled that way.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/model/identity.hpp"
#include "core/util/cancel.hpp"
#include "core/util/process.hpp"

namespace mpi::discovery {

// Something that could be started. Not a device: it has no device id until it
// runs, which is the distinction the `identifier` field's name keeps.
struct BootTarget {
  model::Platform platform = model::Platform::kUnknown;
  // An AVD name on Android, a simulator UDID on iOS.
  std::string identifier;
  std::string display_name;
  // The OS the target will run, when the provider states it. Empty when it
  // does not -- an AVD's API level is not in `emulator -list-avds` output.
  std::string os_version;
  // True when this target is already running, in which case booting it is a
  // no-op rather than an error.
  bool already_running = false;
  std::string provider;

  json::Value to_json() const;
};

struct BootTargets {
  std::vector<BootTarget> targets;
  // Per-provider failures. A provider that could not be asked is reported
  // rather than contributing an empty list that reads as "none exist".
  std::vector<std::string> errors;

  json::Value to_json() const;
};

struct BootOptions {
  // How long to wait for the device to answer after starting it. An emulator
  // cold-booting an API 37 image took about 30 s on the machine this was
  // written on; the default leaves room for a slower one.
  std::chrono::milliseconds ready_timeout{180000};
  std::chrono::milliseconds poll_interval{2000};
  // Paths, so a caller can point at a specific SDK.
  std::string adb_path = "adb";
  std::string emulator_path;  // empty: resolved from ANDROID_HOME / PATH
  CancellationToken cancel;
};
/// The timeout to give the next poll, so `ready_timeout` is the real bound.
///
/// The wait loops used to check the budget *before* each call and then give
/// that call its own fixed timeout. With a 180 s budget and a 60 s per-call
/// cap, a poll starting at 179 s ran until 239 s -- a third longer than the
/// number the caller set, and the overshoot grew with how badly the tooling
/// was behaving. The observed case is a wedged `CoreSimulatorService`, where
/// every `simctl list` hangs for its full timeout: the budget bought three
/// polls instead of ninety, and took four minutes to do it.
///
/// So each call is given whatever is left, capped. Returns `std::nullopt`
/// when too little remains to be worth spawning a process for -- starting a
/// 40 ms poll cannot learn anything and the spawn costs more than that.
std::optional<std::chrono::milliseconds> poll_budget(
    std::chrono::milliseconds elapsed,
    std::chrono::milliseconds ready_timeout,
    std::chrono::milliseconds per_call_cap,
    std::chrono::milliseconds floor);


struct BootResult {
  bool started = false;
  // True only when the device answered. `started && !ready` is a real
  // outcome: the process launched and the device never came up.
  bool ready = false;
  bool was_already_running = false;
  // The device id observed after booting, empty when none could be
  // confirmed. Never inferred from the target's name.
  std::string device_id;
  std::chrono::milliseconds waited{0};
  std::string error;
  std::vector<std::string> notes;

  json::Value to_json() const;
};

// Lists what could be started, per platform.
BootTargets list_boot_targets(const BootOptions& opts);

// Starts `target` and waits until it answers.
//
// Booting something already running is reported as such and waits for nothing.
BootResult boot(const BootTarget& target, const BootOptions& opts);

// Finds the `emulator` binary: `opts.emulator_path`, then ANDROID_HOME /
// ANDROID_SDK_ROOT, then PATH. Empty when it cannot be found, which is
// reported rather than guessed at.
std::string resolve_emulator_path(const BootOptions& opts);

}  // namespace mpi::discovery
