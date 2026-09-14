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
  // `dumpsys gfxinfo PKG framestats` drains the platform's frame buffer on
  // each read, and the collector resets it at capture start so the window
  // contains only this capture's frames. Clearing this flag keeps whatever the
  // platform has already accumulated, which is what you want when the frames
  // of interest were produced before the capture began.
  bool reset_frame_history = true;
  bool cpu_samples = true;
  bool memory = true;
  // Interval between memory counter reads. Each read is a separate process
  // invocation, so this is also the collector's own overhead cadence.
  std::chrono::milliseconds memory_interval{500};
  // A preset name recorded in the trace so two runs can be compared only when
  // they used the same collector configuration (spec I17).
  std::string preset = "lightweight";
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

  bool ok() const { return started && any_data && error.empty(); }
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
};

}  // namespace mpi::session
