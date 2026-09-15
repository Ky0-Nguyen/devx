// `/usr/bin/sample`'s call graph, turned into samples.
//
// This exists because the previous conclusion -- that stack attribution was
// impossible for a simulator app, since `task_for_pid` is refused -- was
// wrong. `sample` is entitled to do what this process cannot, and returns a
// symbolised call graph for a simulator app with no root and no Full Disk
// Access.
//
// == What the tool gives, and what that costs the model ==
//
// `sample` reports an **aggregate**, not a time series: one tree per thread,
// each node carrying the number of samples that passed through it. There are
// no timestamps, so nothing here can be placed on a timeline, and this can
// never answer "when". It answers "where", which is what a hotspot rule
// needs.
//
// Each node's count includes its children. The interesting quantity is
// therefore `self = count - sum(children)`: the samples that stopped *in*
// that frame. A flatten that emitted only leaves would lose every frame with
// both self time and callees, and one that emitted every node at its full
// count would multiply-count the same samples down the whole path.
//
// So one weighted sample is emitted per node with self time, its weight being
// that self count, and its frames being the path from the root. `CpuSample`
// already carries an optional weight for exactly this: a provider that
// reports aggregates rather than individual samples.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/trace.hpp"

namespace mpi::ios {

/// One thread's section of the call graph.
struct SampleThread {
  /// `Thread_6788599` as printed. Not a tid: it is sample's own identifier,
  /// and claiming it was a tid would be a false identifier.
  std::string label;
  /// The rest of the thread line, when there is one -- "DispatchQueue_1:
  /// com.apple.main-thread (serial)". Empty for an unnamed thread, never
  /// guessed at.
  std::string description;
  std::int64_t total_samples = 0;
  bool is_main = false;
};

struct SampleParse {
  bool ok = false;
  std::string error;
  std::vector<SampleThread> threads;
  /// One entry per frame that had self time, outermost frame first.
  std::vector<model::CpuSample> samples;
  /// Lines inside the call graph that could not be read. Counted, never
  /// guessed at -- a format this does not understand must be visible rather
  /// than silently dropped.
  std::int64_t unparsed_lines = 0;
  /// The sum of every emitted weight. It should equal the sum of the threads'
  /// totals; a caller can check that, and a mismatch means frames were lost.
  std::int64_t attributed_samples = 0;
};

/// Parses `sample`'s stdout.
///
/// `process_instance_id` and the per-thread ids are stamped onto the samples
/// so they join the rest of the model.
SampleParse parse_sample_output(const std::string& text,
                                const std::string& process_instance_id);

}  // namespace mpi::ios
