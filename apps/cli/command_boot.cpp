// `mpi boot` -- start a simulator or emulator, and wait until it answers.
//
// Discovery lists what is there; this starts something that is not. It is a
// separate command because it changes the machine's state rather than
// observing it, and because its honest output is different: "started" and
// "ready" are two facts, and a device that was asked to boot and did not come
// up is a real outcome rather than an error to hide.
#include <iomanip>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/discovery/boot.hpp"

namespace mpi::cli {

ExitCode cmd_boot(const Invocation& inv) {
  discovery::BootOptions opts;
  opts.cancel = inv.global.cancel;
  if (inv.has_flag("ready-timeout-s")) {
    const int secs = std::atoi(inv.flag("ready-timeout-s").c_str());
    if (secs <= 0) {
      std::cerr << "error: --ready-timeout-s must be a positive integer\n";
      return ExitCode::kUsage;
    }
    opts.ready_timeout = std::chrono::milliseconds(secs * 1000);
  }

  const bool listing = inv.has_flag("list") || inv.global.device.empty();
  const auto targets = discovery::list_boot_targets(opts);

  if (listing) {
    if (inv.global.json) {
      print_json(targets.to_json());
      return ExitCode::kOk;
    }
    std::cout << "PLATFORM  STATE          OS        IDENTIFIER"
                 "                              NAME\n";
    for (const auto& t : targets.targets) {
      std::cout << std::left << std::setw(10)
                << model::to_string(t.platform) << std::setw(15)
                << (t.already_running ? "running" : "not running")
                << std::setw(10) << (t.os_version.empty() ? "-" : t.os_version)
                << std::setw(40) << t.identifier << t.display_name << "\n";
    }
    if (targets.targets.empty()) {
      std::cout << "(nothing bootable was found)\n";
    }
    for (const auto& e : targets.errors) warn(e);
    // An empty list with a provider error is not "no devices exist", and the
    // exit code says which happened.
    if (targets.targets.empty() && !targets.errors.empty()) {
      return ExitCode::kInconclusive;
    }
    if (inv.global.device.empty() && !inv.has_flag("list")) {
      std::cerr << "\nnote: pass --device <identifier> to boot one of these. "
                   "An AVD name is not a device id: the device id only exists "
                   "once it is running, and this command reports the one it "
                   "observed.\n";
    }
    return ExitCode::kOk;
  }

  // Resolve the requested identifier against what can be booted, so a typo is
  // an error rather than a silent attempt.
  const discovery::BootTarget* chosen = nullptr;
  for (const auto& t : targets.targets) {
    if (t.identifier == inv.global.device) chosen = &t;
  }
  if (chosen == nullptr) {
    std::cerr << "error: '" << inv.global.device
              << "' is not a bootable target on this host. `mpi boot --list` "
                 "shows what is.\n";
    for (const auto& e : targets.errors) warn(e);
    return ExitCode::kNotFound;
  }

  if (!inv.global.quiet) {
    std::cerr << "booting " << chosen->identifier << " ("
              << model::to_string(chosen->platform) << ")";
    if (chosen->already_running) std::cerr << " -- already running";
    std::cerr << "\n";
  }

  const auto result = discovery::boot(*chosen, opts);
  if (inv.global.json) {
    json::Value out = result.to_json();
    out.set("target", chosen->to_json());
    print_json(out);
  } else {
    if (result.was_already_running) {
      std::cout << "already running: " << result.device_id << "\n";
    } else if (result.ready) {
      std::cout << "ready: " << result.device_id << " after "
                << result.waited.count() << " ms\n";
    } else if (result.started) {
      std::cout << "started, NOT confirmed ready after "
                << result.waited.count() << " ms\n";
    } else {
      std::cout << "not started\n";
    }
    for (const auto& n : result.notes) std::cerr << "note: " << n << "\n";
    if (!result.error.empty()) std::cerr << "error: " << result.error << "\n";
  }

  if (!result.started) return ExitCode::kCollectionError;
  // Started but never ready is inconclusive, not success: a caller that
  // records straight after this would capture a booting device.
  if (!result.ready) return ExitCode::kInconclusive;
  return ExitCode::kOk;
}

}  // namespace mpi::cli
