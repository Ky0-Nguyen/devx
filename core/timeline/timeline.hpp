// A binned timeline for the UI, and the one honesty problem a chart creates
// that a table does not.
//
// A table can leave a cell empty. A track cannot: every pixel of a rendered
// lane says *something*, and the default reading of a flat lane is "nothing
// happened". That reading is wrong whenever the collector was not running,
// and the specification's first rule is that missing data is not zero
// (section 0.2, H01). So a bin here does not hold a number. It holds a state
// and an optional number:
//
//   kMeasured    the collector covered this bin. `value` is present, and a
//                present 0 means a measured zero -- the app really drew no
//                frames.
//   kNoReading   the collector covered this bin but took no reading in it.
//                Only a sampled quantity can be in this state, and it is the
//                normal case between two samples: memory did not fall to zero
//                between readings, so filling the bin with 0 would invent a
//                measurement. An earlier version did exactly that and drew a
//                878 MiB process as a flat line at zero.
//   kPartial     the collector covered the bin but a gap intersects it. The
//                value is present and is wrong-low by an unknown amount, so
//                the covered fraction travels with it.
//   kUnmeasured  nothing covered this bin. `value` is absent and the reason
//                is recorded. A renderer must draw this differently from a
//                measured zero; drawing it as zero is the bug this type
//                exists to prevent.
//
// The second problem is placement. A JS task or an SDK marker is stamped on
// the app's own clock, and putting it on the device's timeline without a
// measured mapping would line up two unrelated clocks and make the result
// look like evidence. `NormalizedTrace::map_to_primary` already refuses that;
// this follows it. A track whose events cannot be placed is still emitted --
// with no bins, `placed` false, and the reason -- because a missing track and
// a track that could not be positioned are different facts (H11).
//
// Binning is bounded on purpose: a 1 GiB capture and a 1 MiB one produce the
// same number of bins, so the UI's cost does not scale with the capture's.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/util/cancel.hpp"

namespace mpi::timeline {

enum class BinState {
  kUnmeasured,  // no coverage: value absent
  kNoReading,   // covered, but this sampled quantity was not read here
  kPartial,     // covered, but a gap intersects this bin
  kMeasured,    // fully covered
};
const char* to_string(BinState s);

struct Bin {
  model::TimeNs start_ns = 0;
  model::TimeNs end_ns = 0;
  BinState state = BinState::kUnmeasured;
  // Absent whenever `state` is kUnmeasured or kNoReading. Present and zero
  // means measured zero. These are different facts and the type keeps them
  // apart.
  std::optional<double> value;
  std::int64_t event_count = 0;
  // How much of the bin the collector actually covered, when that is less
  // than all of it.
  std::optional<double> covered_fraction;
  json::Value to_json() const;
};

// What a track's number means, which decides how it may be drawn and what it
// may be compared against.
enum class TrackKind {
  kOccurrence,  // a count of events in the bin (frames, samples)
  kDuration,    // summed duration of intervals overlapping the bin
  kSeries,      // a sampled quantity: the last reading in the bin, not a sum
  kFlag,        // a count of a specific condition (missed deadlines)
};
const char* to_string(TrackKind k);

struct Track {
  std::string id;
  std::string label;
  std::string unit;
  std::string provider;
  TrackKind kind = TrackKind::kOccurrence;
  // The collectors whose coverage decides this track's bin states, as the
  // providers actually named them. A source kind has several possible
  // provider names across platforms, and picking the wrong one silently
  // turned every bin unmeasured -- so what was matched is reported.
  std::vector<std::string> collectors;
  // Why the bin states are what they are: which coverage row was used, or
  // that none was found. A track with no coverage row is a different fact
  // from a track whose collector reported a gap, and the UI has to be able
  // to say which (H11).
  std::string coverage_note;
  // The clock the source events are stamped on, and whether they could be
  // placed on the capture's timeline.
  std::string clock_domain;
  bool placed = true;
  std::string placement_note;
  // What this track's numbers do not say. Carried per track rather than
  // written into the UI, so the constraint travels with the data.
  std::vector<std::string> limitations;
  std::vector<Bin> bins;
  // Over measured bins only, so an unmeasured stretch cannot lower a peak.
  std::optional<double> max_value;
  std::int64_t total_events = 0;
  // Events the source produced that are not on this timeline, with why.
  std::int64_t unplaced_events = 0;
  json::Value to_json() const;
};

// An exact interval, unbinned: what an issue or a gap actually covers. The
// UI needs these at full resolution because "issue click focuses the actual
// evidence interval" (spec section 13) means the interval, not its bin.
struct Band {
  std::string id;
  std::string label;
  model::TimeNs start_ns = 0;
  model::TimeNs end_ns = 0;
  std::string detail;
  // For an issue band: whether the interval is the evidence's own or was
  // widened to be visible. A zero-width interval cannot be clicked, so it is
  // drawn wider -- and says so, because a widened band would otherwise imply
  // a duration that was never measured.
  bool widened = false;
  json::Value to_json() const;
};

struct Timeline {
  std::string session_id;
  model::TimeNs window_start_ns = 0;
  model::TimeNs window_end_ns = 0;
  model::TimeNs bin_width_ns = 0;
  int bin_count = 0;
  std::string primary_clock_domain;
  std::vector<Track> tracks;
  std::vector<Band> issues;
  std::vector<Band> gaps;
  // Set when the capture has no window at all, in which case there is
  // nothing to bin and the UI must say so rather than draw an empty axis.
  std::string empty_reason;
  json::Value to_json() const;
};

struct Options {
  int bin_count = 240;
  // A band narrower than this is widened to it so it can be clicked, and
  // flagged as widened.
  model::TimeNs min_band_ns = 0;
  CancellationToken cancel;
};

// Pure: no I/O, no device. Everything it reports comes from the trace and the
// analysis it is handed.
Timeline build(const model::NormalizedTrace& trace,
               const std::vector<model::Issue>& issues,
               const Options& opts);

}  // namespace mpi::timeline
