// Live capture contract.
//
// A collector turns a selected target into a NormalizedTrace. It is the only
// place in the system that talks to a running app, and the only place allowed
// to say a measurement was taken.
//
// Three rules the interface enforces rather than documents:
//
//  * A collector reports what it actually collected, per source, as a
//    Capability. A source it could not start becomes a coverage gap with a
//    reason, never an absence of events.
//  * A collector never invents a value. If the provider gave no presentation
//    timestamp, the FrameRecord carries none.
//  * Cancellation is cooperative and must leave the device as it was found:
//    spec D22 forbids cleanup from touching anything the session does not own.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "core/model/capability.hpp"
#include "core/model/trace.hpp"
#include "core/util/cancel.hpp"

namespace mpi::session {

// Which sources to attempt. Each is independent: one failing must not prevent
// the others, and the result records which actually ran.
struct CaptureConfig {
  std::chrono::milliseconds duration{5000};
  int sample_frequency_hz = 200;
  bool frames = true;
  // `dumpsys gfxinfo PKG framestats` is a ring buffer of roughly the last 120
  // frames and does NOT drain when read, so the collector clears it at capture
  // start to keep the window to this capture's frames. Clearing this flag
  // keeps whatever the platform has already accumulated, which is what you
  // want when the frames of interest were produced before the capture began.
  bool reset_frame_history = true;
  bool cpu_samples = true;
  bool memory = true;
  // Scheduling and I/O evidence from `atrace`. Off by default, and
  // deliberately so: the trace is **system-wide**, it costs a kernel buffer
  // and CPU on the device, and it is the one source here that observes
  // processes other than the target. It is enabled per capture by an
  // operator who wants it (spec section 13: explain overhead before enabling
  // a heavier collector).
  bool scheduling = false;
  // Take a heap dump at the end of the capture. Off by default and expensive
  // in a way the operator should choose: `am dumpheap` pauses the app while
  // it walks the whole heap, and the file it writes is tens of megabytes --
  // 49 MB for the app this was built against.
  bool heap_dump = false;

  // Take a stack profile at the end of the capture, on a platform where that
  // is a separate step from the tick loop.
  //
  // Off by default because it costs its own duration: the iOS simulator path
  // runs `/usr/bin/sample`, which blocks for the seconds it samples. It is
  // also a different *kind* of result from the rest of a live capture -- an
  // aggregate with no timestamps, which answers "where" and can never
  // answer "when" -- and mixing that into a timeline by default would invite
  // reading it as one.
  bool stack_profile = false;
  std::chrono::milliseconds stack_profile_duration{3000};
  // Where a collector may write a large side-artifact it cannot hold in
  // memory -- a heap dump is tens of megabytes. The session writer moves what
  // it finds here into the package.
  std::string artifact_dir;
  // Kernel buffer per CPU, in kilobytes. Too small and the kernel drops
  // events, which is reported as a gap rather than absorbed.
  int scheduling_buffer_kb = 16384;
  // Interval between memory counter reads. Each read is a separate process
  // invocation, so this is also the collector's own overhead cadence.
  std::chrono::milliseconds memory_interval{500};
  // A preset name recorded in the trace so two runs can be compared only when
  // they used the same collector configuration (spec I17).
  std::string preset = "lightweight";
  // Streaming cadence for the cheap sources. Measured on a real emulator,
  // `dumpsys gfxinfo framestats` costs ~50 ms and `dumpsys meminfo` ~130 ms,
  // so a few hundred milliseconds is comfortable.
  std::chrono::milliseconds tick_interval{500};
  // CPU sampling window, which is deliberately *not* on the tick path.
  // simpleperf costs roughly 5.6 s of fixed overhead per record-and-symbolise
  // cycle regardless of window length, so putting it on a 500 ms tick would
  // make the tick take 6 s and the cadence meaningless. Instead it runs on its
  // own thread in windows of this length, and the intervals between windows
  // are recorded as coverage gaps rather than as measured idle time.
  std::chrono::milliseconds cpu_window{5000};
  // Zero means run until stopped, which is what a live session does.
  bool run_until_stopped = false;

  // Launch the app as part of the capture, so the startup itself is measured
  // rather than requiring the operator to have started it already.
  bool launch_app = false;
  // "cold" force-stops first so the process starts from nothing; "warm" and
  // "hot" leave whatever is running in place. The platform reports its own
  // classification of what actually happened, and that -- not this request --
  // is what gets recorded.
  std::string launch_class = "cold";
  // How long to wait for the app's process to appear after a launch. Zero
  // means do not wait.
  std::chrono::milliseconds wait_for_app{0};
  CancellationToken cancel;

  json::Value to_json() const;
};

struct CaptureResult {
  bool started = false;
  // True when at least one source produced data.
  bool any_data = false;
  std::string error;
  // One entry per attempted source, whether it worked or not.
  std::vector<model::Capability> source_results;
  // Measured wall time of the capture itself, for overhead accounting.
  std::chrono::milliseconds elapsed{0};
  // Large artifacts written to `CaptureConfig::artifact_dir`, as
  // {name inside the package's raw/, absolute path on disk}. Kept out of the
  // trace because a heap dump does not belong in a JSON document.
  std::vector<std::pair<std::string, std::string>> artifacts;

  bool ok() const { return started && any_data && error.empty(); }
};

// What one streaming tick added.
//
// A UI polls these to show a capture as it happens. The counts are deltas, and
// the per-source status is the *current* state, so a source that starts
// failing mid-capture shows up immediately rather than at the end.
struct LiveUpdate {
  model::TimeNs at_ns = 0;
  std::int64_t new_frames = 0;
  std::int64_t new_cpu_samples = 0;
  std::int64_t new_counter_points = 0;
  // Wall time this tick's collection itself took. This is collector overhead
  // and is reported rather than hidden: spec section 9 rule 10 wants it
  // measured, and a UI showing live numbers should be able to show its cost.
  std::chrono::milliseconds tick_cost{0};
  std::vector<model::Capability> source_status;
  std::vector<std::string> notes;

  json::Value to_json() const;
};

class Collector {
 public:
  virtual ~Collector() = default;
  virtual std::string id() const = 0;
  virtual model::Platform platform() const = 0;

  // Captures into `out`, which must already carry the pinned target identity.
  // Returns what happened per source; a partial capture is a success with
  // gaps, not a failure.
  virtual CaptureResult capture(const model::DeviceRef& device,
                                const std::vector<model::ProcessInstance>& processes,
                                const CaptureConfig& config,
                                model::NormalizedTrace& out) = 0;

  // ---- streaming -----------------------------------------------------------
  //
  // A streaming collector collects in increments so a capture can be watched
  // while it runs. `begin` prepares the device, `tick` collects one increment
  // into `out`, and `finish` closes the window and fills coverage.
  //
  // Streaming is not a different measurement: the same sources produce the
  // same events. What differs is cadence, and cadence costs -- each tick is
  // process invocations against the device, which `LiveUpdate::tick_cost`
  // reports.
  // What the platform reported about a launch this collector performed.
  //
  // Durations stay absent unless a launch actually happened: a platform that
  // prints a zero because nothing was started must not hand back a
  // zero-millisecond startup (see `parse_am_start_w`).
  struct LaunchReport {
    bool supported = false;
    bool started = false;
    std::string error;
    // The platform's own classification of the launch, e.g. COLD. Never
    // inferred from how long it took.
    std::string launch_class;
    std::optional<model::TimeNs> total_time_ns;
    std::optional<model::TimeNs> wait_time_ns;
    std::optional<model::TimeNs> displayed_ns;
    std::vector<model::Capability> source_results;
    std::vector<std::string> notes;
  };

  // Launches the target and records what the platform reported about the
  // startup into `out`. The default is an honest refusal: a collector that
  // cannot launch says so rather than pretending the app was already there.
  virtual LaunchReport launch(const model::DeviceRef& /*device*/,
                              const std::string& /*app_identifier*/,
                              const CaptureConfig& /*config*/,
                              model::NormalizedTrace& /*out*/) {
    LaunchReport r;
    r.supported = false;
    r.error = "this collector cannot launch an app; start it on the device and "
              "record without --launch";
    return r;
  }

  // Maps a host instant onto the device clock.
  //
  // It exists for producers whose timestamps are not the device's -- the app
  // SDK stamps markers with the app's own clock -- so the two can be related
  // by measurement instead of by assumption. Absent when this collector
  // anchored no device clock, in which case nothing may be mapped and the
  // rules that need both timelines say so.
  struct DeviceClock {
    std::string domain;
    model::TimeNs at_ns = 0;
    // Half-width of the pairing, carried so a caller records a mapping with
    // its precision rather than as an exact offset.
    model::TimeNs uncertainty_ns = 0;
  };
  virtual std::optional<DeviceClock> device_clock_at(
      std::chrono::steady_clock::time_point /*host_instant*/) const {
    return std::nullopt;
  }

  virtual bool supports_streaming() const { return false; }

  virtual CaptureResult begin(const model::DeviceRef& /*device*/,
                              const std::vector<model::ProcessInstance>& /*processes*/,
                              const CaptureConfig& /*config*/,
                              model::NormalizedTrace& /*out*/) {
    CaptureResult r;
    r.error = "this collector does not support streaming";
    return r;
  }
  virtual LiveUpdate tick(const model::DeviceRef& /*device*/,
                          const std::vector<model::ProcessInstance>& /*processes*/,
                          const CaptureConfig& /*config*/,
                          model::NormalizedTrace& /*out*/) {
    return LiveUpdate{};
  }
  virtual CaptureResult finish(const model::DeviceRef& /*device*/,
                               const std::vector<model::ProcessInstance>& /*processes*/,
                               const CaptureConfig& /*config*/,
                               model::NormalizedTrace& /*out*/) {
    CaptureResult r;
    r.error = "this collector does not support streaming";
    return r;
  }
};

}  // namespace mpi::session
