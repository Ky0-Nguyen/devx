// `adb shell atrace` output: the ftrace text format.
//
// This is the route to scheduling and I/O evidence that does not require
// Perfetto. ADR-0006 chose Android's text interfaces over a protobuf
// dependency, and `atrace` without `-z`/`--compress` prints plain ftrace,
// which carries exactly what the two missing detectors need:
//
//   sched_switch          prev_state, so a thread's own state is the
//                         platform's word rather than an inference. `D` is
//                         uninterruptible sleep -- usually disk -- and `R`
//                         means runnable but not running, which is the
//                         difference E10 asks for.
//   sched_blocked_reason  `iowait=` and the kernel `caller=`, which is the
//                         only evidence here that a block was I/O at all.
//   sched_waking          who made a thread runnable, which is what a lock
//                         owner claim needs (DET-09) instead of a guess.
//   tracing_mark_write    `B|pid|name` / `E|pid`: the app's and the
//                         framework's own slices, so a blocked stretch can be
//                         named rather than just timed.
//   trace_event_clock_sync  a measured pairing of the trace clock to both the
//                         boot clock and wall time.
//
// Two things about this provider shape the parser.
//
// It is **system-wide**. Every process on the device is in the file, so
// nothing may be attributed to the app without its thread ids. The parser
// reports what it read; the caller decides what belongs to the target.
//
// Its buffer can **overflow**, and the header says so: ftrace prints
// `entries-in-buffer/entries-written`, and a difference means events the
// kernel dropped. That is a coverage gap, not a quiet period, and it is
// surfaced rather than absorbed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/model/trace.hpp"

namespace mpi::android {

// What a thread was doing when it stopped running, as the kernel reported it.
enum class ThreadState {
  kRunning,          // still runnable, preempted: "R" / "R+"
  kSleeping,         // "S": waiting on something, interruptibly
  kUninterruptible,  // "D": usually disk, but only `iowait` proves it
  kIdleKernel,       // "I": an idle kernel worker
  kDead,             // "X" / "Z"
  kOther,
};
const char* to_string(ThreadState s);
ThreadState thread_state_from_ftrace(const std::string& token);

struct SchedSwitch {
  model::TimeNs timestamp_ns = 0;
  int cpu = 0;
  std::int32_t prev_tid = 0;
  std::string prev_comm;
  ThreadState prev_state = ThreadState::kOther;
  std::string prev_state_raw;
  std::int32_t next_tid = 0;
  std::string next_comm;
  // The thread group the *emitting* line belonged to, from the `(TGID)`
  // column. Absent when ftrace printed `(-------)`.
  std::optional<std::int32_t> tgid;
};

struct BlockedReason {
  model::TimeNs timestamp_ns = 0;
  std::int32_t tid = 0;
  // The kernel's own flag. Without it a `D` state says "blocked", not
  // "blocked on I/O", and the difference is the whole of DET-03.
  bool iowait = false;
  std::string caller;
  std::optional<std::int32_t> tgid;
};

struct SchedWaking {
  model::TimeNs timestamp_ns = 0;
  // The thread that performed the wake-up: the line's own emitter.
  std::int32_t waker_tid = 0;
  std::string waker_comm;
  std::int32_t target_tid = 0;
  std::string target_comm;
};

// A userspace slice from `Trace.beginSection` / `endSection`, which both the
// framework and the app emit.
struct Slice {
  model::TimeNs timestamp_ns = 0;
  bool begin = false;
  std::int32_t tid = 0;   // the emitting thread
  std::int32_t pid = 0;   // the pid in the marker payload
  std::string name;       // empty for an end marker
};

// The pairing ftrace emits for its own clock. `parent_ts` is the trace clock
// (boot-relative seconds) and `realtime_ts` is wall time in ms.
struct AtraceClockSync {
  bool have_parent = false;
  bool have_realtime = false;
  model::TimeNs parent_ns = 0;
  model::TimeNs realtime_ns = 0;
};

struct AtraceTrace {
  // From the ftrace header. Equal counts mean nothing was dropped.
  std::int64_t entries_in_buffer = 0;
  std::int64_t entries_written = 0;
  bool header_seen = false;
  // `entries_written > entries_in_buffer`: the kernel dropped events.
  std::int64_t dropped_events = 0;

  AtraceClockSync clock_sync;
  std::vector<SchedSwitch> switches;
  std::vector<BlockedReason> blocked;
  std::vector<SchedWaking> wakings;
  std::vector<Slice> slices;

  model::TimeNs first_ns = 0;
  model::TimeNs last_ns = 0;
  std::int64_t lines_read = 0;
  std::int64_t lines_unrecognised = 0;
  std::vector<std::string> warnings;
};

// Parses ftrace text. Unrecognised lines are counted, never guessed at.
AtraceTrace parse_atrace(const std::string& text);

}  // namespace mpi::android
