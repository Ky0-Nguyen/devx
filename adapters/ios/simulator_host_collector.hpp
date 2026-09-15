// Live capture for an app running in an iOS **simulator**.
//
// `xctrace` cannot stream: it produces a trace bundle when it finishes, so
// `XctraceCollector::supports_streaming()` is false and always will be. That
// left iOS with no live capture at all, and the Live tab refusing every
// attempt -- which is the correct answer for a physical device and an
// unnecessary one for a simulator.
//
// A simulator is different in a way that makes this possible: the app runs as
// an ordinary macOS process owned by the developer. No `task_for_pid`, no
// root, no entitlement -- the public `libproc` interfaces answer for a
// same-user process, and they answer at a resolution worth sampling.
//
// == The unit trap ==
//
// The task-level CPU counters (`PROC_PIDTASKINFO`, and the identical values
// in `proc_pid_rusage`) are in **mach absolute time units**, not nanoseconds.
// The per-thread counters (`PROC_PIDTHREADINFO`) *are* nanoseconds. Two units
// in one API family.
//
// Read as nanoseconds, a process that had burned 3.0 s of CPU reported
// 0.072 s -- a factor of 41.67, which is the Apple Silicon timebase
// (numer 125 / denom 3). With the timebase applied the same reading is
// 2.999 s, against 3.00 s from `ps` and 3.0 s of deliberately burned CPU.
// That discrepancy is why this collector did not exist until now: a CPU
// series fifty times too small is worse than no CPU series.
//
// == What it does not provide ==
//
// **No frames.** There is no command-line source for a simulator's frame
// timing; Core Animation's counters are not exposed and the simulator does
// not render through the device's display pipeline anyway. Frames are
// reported as a missing provider, never as zero, and a capture here cannot
// support any frame-deadline finding.
//
// **No stacks.** Stack sampling needs `task_for_pid`, which is refused
// without root or the debugger entitlement (verified: kr=5). So there are CPU
// *times*, per process and per thread, and no attribution to functions.
//
// **Simulator timings are not device timings.** Already true of every
// simulator capture in this tool, and more so here: the app may be running
// translated under Rosetta, which the collector detects and records, because
// translated CPU time is not comparable to anything.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/session/collector.hpp"

namespace mpi::ios {

/// One reading of a host process.
struct HostProcessSample {
  bool ok = false;
  std::string error;
  /// Total CPU, already converted from mach units to nanoseconds.
  std::int64_t cpu_time_ns = 0;
  /// `phys_footprint`: the number Xcode's memory gauge shows.
  std::int64_t footprint_bytes = 0;
  std::int64_t resident_bytes = 0;
  std::int32_t thread_count = 0;
};

struct HostThreadSample {
  std::uint64_t handle = 0;
  std::string name;          // empty for an unnamed thread; never guessed at
  std::int64_t cpu_time_ns = 0;   // per-thread counters are already ns
};

/// Reads a process's CPU and memory. Public for tests and for the iOS
/// reconnaissance documented in docs/ios-live-capture-findings.md.
HostProcessSample sample_host_process(std::int32_t pid);
std::vector<HostThreadSample> sample_host_threads(std::int32_t pid);

/// Converts a mach-absolute-time duration to nanoseconds.
///
/// Its own function because it is the single point where the unit trap above
/// is handled, and because a test can check it against the timebase rather
/// than against a hard-coded 41.6667.
std::int64_t mach_ticks_to_ns(std::uint64_t ticks);

/// Finds the host pid of an app running in a simulator.
///
/// `simctl spawn <udid> launchctl list` prints the simulator's launchd jobs,
/// and an app appears as `UIKitApplication:<bundle-id>[…]` with its **host**
/// pid in the first column. That is the authoritative mapping; matching
/// process names would pick the wrong simulator when two run the same app.
struct PidLookup {
  bool found = false;
  std::int32_t pid = 0;
  std::string error;
};
PidLookup find_simulator_app_pid(const std::string& udid,
                                 const std::string& bundle_id,
                                 std::chrono::milliseconds timeout);

/// Whether the process is running translated under Rosetta.
///
/// Recorded rather than ignored: translated CPU time is not comparable to a
/// native build, let alone to a device, and a capture that did not say so
/// would invite exactly that comparison.
std::optional<bool> is_translated(std::int32_t pid);

class SimulatorHostCollector : public session::Collector {
 public:
  std::string id() const override { return "simctl-host"; }
  model::Platform platform() const override { return model::Platform::kIos; }

  session::CaptureResult capture(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

  bool supports_streaming() const override { return true; }

  session::CaptureResult begin(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;
  session::LiveUpdate tick(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;
  session::CaptureResult finish(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

 private:
  void add_point(model::NormalizedTrace& out, const std::string& name,
                 const std::string& family, const std::string& unit,
                 model::TimeNs at_ns, double value);

  std::int32_t pid_ = 0;
  std::string process_key_;
  std::string bundle_id_;
  bool started_ = false;
  std::optional<bool> translated_;
  /// The previous reading, for the utilisation rate. A rate needs two
  /// readings, so the first tick publishes no utilisation rather than 0%.
  std::optional<std::int64_t> last_cpu_ns_;
  std::optional<model::TimeNs> last_at_ns_;
  std::int64_t ticks_ = 0;
  std::int64_t dead_ticks_ = 0;
  std::map<std::uint64_t, std::string> known_threads_;
};

}  // namespace mpi::ios
