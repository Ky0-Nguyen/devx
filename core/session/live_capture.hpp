// A capture you can watch while it runs.
//
// The batch path answers "what happened during those six seconds". This
// answers "what is happening", which is a different question and needs a
// different shape: a collector ticking on a background thread, a trace that
// grows, and a snapshot the UI can read at any moment without waiting.
//
// The specification is explicit about the cost of that (section 2.2:
// post-capture analysis first, live preview *explicitly preliminary*; section
// 13: live results are preliminary). So every snapshot says so, and the
// analysis attached to one is marked preliminary in the document itself rather
// than by a label in the UI that could be dropped in translation.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/rules/engine.hpp"
#include "core/session/collector.hpp"
#include "core/symbols/symbol_service.hpp"

namespace mpi::session {

enum class LiveState {
  kIdle,
  kStarting,
  kRunning,
  kStopping,
  kFinished,
  kFailed,
};
const char* to_string(LiveState s);

// What a caller sees at one instant. Copied out under the lock, so a UI can
// render it without holding anything.
struct LiveSnapshot {
  LiveState state = LiveState::kIdle;
  std::string session_id;
  std::string error;

  std::chrono::milliseconds elapsed{0};
  std::int64_t ticks = 0;
  std::chrono::milliseconds collector_cost{0};
  // Cost of the most recent tick, so a caller can see when the device rather
  // than the configured cadence is setting the pace.
  std::chrono::milliseconds last_tick_cost{0};

  std::int64_t frames = 0;
  std::int64_t cpu_samples = 0;
  std::int64_t counter_points = 0;
  std::int64_t threads = 0;

  model::TimeNs window_start_ns = 0;
  model::TimeNs window_end_ns = 0;

  // The current state of each source, so a source that starts failing
  // mid-capture is visible immediately.
  std::vector<model::Capability> source_status;
  std::vector<std::string> notes;

  // Analysis over what has arrived so far. Always preliminary: the window is
  // still open, so a detector that has found nothing yet may still fire, and
  // one that has fired may look different at the end.
  model::AnalysisResult preliminary_analysis;
  bool analysis_is_preliminary = true;

  // Latest value per memory family, for a live readout that does not require
  // the caller to walk the counter series.
  /// The newest point of each counter, with the unit it is in.
  ///
  /// The unit used not to travel -- this was a (name, value) pair -- and the
  /// Live tab formatted every one of them as bytes. `cpu.process_time_ns` is
  /// nanoseconds, so a two-and-a-half hour capture rendered as "8782.51 GB":
  /// the number was right and the unit was a fabrication. A view cannot pick
  /// a formatter it was never told the unit for.
  struct LatestCounter {
    std::string name;
    double value = 0;
    std::string unit;
    // Whether `value` is a running total or a reading at that instant.
    bool cumulative = false;
    // What a percentage is a percentage of, as declared by the series
    // (spec section 8 / E12). "not_applicable" for everything that is not
    // one.
    std::string cpu_normalization;
    // For a cumulative counter, the change across the points this capture
    // actually collected, and the span they cover.
    //
    // This is the number a reader of a live capture wants: `value` for
    // `cpu.process_time_ns` is the process's whole life, so a 67 s window on
    // an app that had been running for hours read 3.18 h. The delta is what
    // the window cost.
    //
    // Absent when the series has fewer than two points, because a difference
    // needs two -- not zero, which would claim the app used no CPU.
    std::optional<double> delta;
    std::optional<std::int64_t> delta_span_ns;
  };
  std::vector<LatestCounter> latest_counters;

  json::Value to_json() const;
};

// Drives a streaming collector on its own thread.
//
// One session per instance. `start` returns as soon as the collector has
// prepared the device; the ticks continue until `stop`.
class LiveSession {
 public:
  LiveSession();
  ~LiveSession();
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  // Takes ownership of the collector. Returns false and fills the snapshot's
  // error when the collector cannot start.
  bool start(std::shared_ptr<Collector> collector, const model::DeviceRef& device,
             std::vector<model::ProcessInstance> processes, CaptureConfig config,
             model::NormalizedTrace seed);

  // Requests a stop and waits for the thread to finish.
  void stop();

  bool running() const;
  LiveSnapshot snapshot() const;

  // The finished trace, valid only once the state is kFinished. Moves the
  // trace out, because the caller then owns it for analysis and persistence.
  model::NormalizedTrace take_trace();
  CaptureResult result() const;

  // The collector this session is driving, so a caller can ask it something
  // only it knows -- the device clock it anchored, for instance. Held as a
  // shared_ptr for the session's lifetime, so this stays valid as long as the
  // session does.
  std::shared_ptr<Collector> collector() const;

  // How often the preliminary analysis is recomputed. Analysis over a growing
  // trace is not free, so it runs less often than the collector ticks.
  void set_analysis_interval(std::chrono::milliseconds v) {
    analysis_interval_ = v;
  }

 private:
  void run_loop();
  void refresh_snapshot(const LiveUpdate* update, bool run_analysis);

  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  CancellationSource cancel_;

  std::shared_ptr<Collector> collector_;
  model::DeviceRef device_;
  std::vector<model::ProcessInstance> processes_;
  CaptureConfig config_;

  model::NormalizedTrace trace_;
  LiveSnapshot snapshot_;
  CaptureResult result_;
  std::chrono::steady_clock::time_point started_at_;
  std::chrono::milliseconds analysis_interval_{1500};
};

}  // namespace mpi::session
