// Post-read normalization: ordering, duplicate detection, span pairing,
// coverage computation, and the derived frame/refresh view.
//
// Everything here is conservative. An unpairable span stays incomplete, an
// out-of-order event keeps its flag, and a window with no events becomes a
// gap rather than a run of zeroes.
#pragma once

#include <string>
#include <vector>

#include "core/model/trace.hpp"
#include "core/util/cancel.hpp"

namespace mpi::ingest {

struct NormalizeReport {
  std::int64_t duplicates_flagged = 0;
  std::int64_t out_of_order_flagged = 0;
  std::int64_t incomplete_spans = 0;
  std::int64_t events_outside_window = 0;
  std::vector<std::string> notes;
};

// Sorts events by timestamp, flags duplicates and originally-out-of-order
// events, pairs B/E spans, derives the capture window, and fills coverage.
// Idempotent: running it twice produces the same trace.
NormalizeReport normalize(model::NormalizedTrace& trace,
                          const CancellationToken& cancel = CancellationToken::none());

// Derives coverage for a collector from the events attributed to it, treating
// any inter-event interval longer than `gap_threshold_ns` as a gap.
model::Coverage derive_coverage(const model::NormalizedTrace& trace,
                                const std::string& collector,
                                model::TimeNs gap_threshold_ns);

}  // namespace mpi::ingest
