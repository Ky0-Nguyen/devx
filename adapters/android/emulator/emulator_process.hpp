// Running Android emulators: finding them, starting one, stopping one.
//
// A running emulator announces itself in a discovery file
// (~/Library/Caches/TemporaryItems/avd/running/pid_<pid>.ini on macOS) that
// names its AVD, its console and adb ports, and its gRPC port and access
// token. That file is how DevX attaches -- to an emulator it started, and to
// one started by Android Studio when that one accepts a token.
//
// DevX starts an emulator the way Android Studio's embedded mode does: its own
// window hidden (-qt-hide-window), gRPC on a loopback port behind a per-launch
// token, so DevX draws the screen and the emulator's Extended Controls window
// can still be opened on request.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::android {

struct RunningEmulator {
  int pid = 0;
  std::string avd_id;
  std::string avd_name;
  std::string serial;  // emulator-<console port>, what adb calls it
  int grpc_port = 0;
  /// The gRPC token. Never serialised: anything that can read it can drive
  /// the device.
  std::string token;
  /// Android Studio launches with JWT auth instead, which DevX cannot sign
  /// for; such an emulator is listed but cannot be attached.
  bool jwt_only = false;
  std::string emulator_version;
  std::string discovery_file;
  bool attachable() const { return grpc_port > 0 && !token.empty(); }
  json::Value to_json() const;
};

std::string discovery_directory();
std::vector<RunningEmulator> running_emulators();
std::optional<RunningEmulator> find_running(const std::string& avd_id);

struct LaunchOptions {
  bool cold_boot = false;
  bool wipe_data = false;
  std::chrono::seconds boot_timeout{240};
  CancellationToken cancel;
  std::function<void(const std::string&)> progress;
};

struct LaunchResult {
  bool ok = false;       // started, attachable, and booted
  bool started = false;  // a process appeared, booted or not
  RunningEmulator emulator;
  std::string error;
  std::string log_path;  // the emulator's own output
  std::vector<std::string> notes;
};

/// Starts `avd_id` with the emulator in `sdk_root` and waits until Android
/// has booted. An AVD already running is attached to, not started twice.
LaunchResult launch_emulator(const std::string& sdk_root, const std::string& avd_id,
                             const LaunchOptions& options);

/// Asks the emulator to shut down (`adb emu kill`), then signals it if it does
/// not go.
bool stop_emulator(const std::string& sdk_root, const RunningEmulator& emulator,
                   std::string* error);

/// A free loopback TCP port at or above `from`.
int free_loopback_port(int from);

}  // namespace mpi::android
