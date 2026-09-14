#include "core/ingestion/normalize.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>

namespace mpi::ingest {

using model::TimeNs;

NormalizeReport normalize(model::NormalizedTrace& trace,
                          const CancellationToken& cancel) {
  NormalizeReport rep;

  // Duplicate event ids. A provider that repeats an id is reporting the same
  // observation twice; flagging both keeps the count honest without deleting
  // data we did not author (spec D10).
  std::unordered_map<std::string, int> id_counts;
  for (const auto& e : trace.events) {
    if (!e.event_id.empty()) ++id_counts[e.event_id];
  }
  for (auto& e : trace.events) {
    if (e.event_id.empty()) continue;
    if (id_counts[e.event_id] > 1) {
      e.add_flag(model::QualityFlag::kDuplicate);
      ++rep.duplicates_flagged;
    }
  }

  // Out-of-order arrival is a property of the capture, so it is recorded
  // before sorting rather than erased by it.
  TimeNs prev = std::numeric_limits<TimeNs>::min();
  for (auto& e : trace.events) {
    if (cancel.cancelled()) {
      rep.notes.push_back("normalization cancelled");
      return rep;
    }
    if (e.timestamp < prev) {
      e.add_flag(model::QualityFlag::kOutOfOrder);
      ++rep.out_of_order_flagged;
    }
    prev = e.timestamp;
  }

  // A timestamp in a domain with no measured mapping to the primary domain
  // cannot be placed on the shared timeline.
  for (auto& e : trace.events) {
    if (e.clock_domain.empty() || e.clock_domain == trace.primary_clock_domain) {
      continue;
    }
    if (!trace.map_to_primary(e.clock_domain, e.timestamp).has_value()) {
      e.add_flag(model::QualityFlag::kClockUnmapped);
    }
  }

  const auto by_time = [](const model::Event& a, const model::Event& b) {
    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
    return a.event_id < b.event_id;  // stable, so fingerprints are stable
  };
  std::stable_sort(trace.events.begin(), trace.events.end(), by_time);

  std::stable_sort(trace.frames.begin(), trace.frames.end(),
                   [](const model::FrameRecord& a, const model::FrameRecord& b) {
                     return a.start_ns < b.start_ns;
                   });
  std::stable_sort(trace.js_tasks.begin(), trace.js_tasks.end(),
                   [](const model::JsTask& a, const model::JsTask& b) {
                     return a.start_ns < b.start_ns;
                   });
  std::stable_sort(trace.markers.begin(), trace.markers.end(),
                   [](const model::Marker& a, const model::Marker& b) {
                     return a.timestamp_ns < b.timestamp_ns;
                   });
  std::stable_sort(trace.cpu_samples.begin(), trace.cpu_samples.end(),
                   [](const model::CpuSample& a, const model::CpuSample& b) {
                     return a.timestamp_ns < b.timestamp_ns;
                   });
  for (auto& c : trace.counters) {
    std::stable_sort(c.points.begin(), c.points.end(),
                     [](const std::pair<TimeNs, double>& a,
                        const std::pair<TimeNs, double>& b) {
                       return a.first < b.first;
                     });
  }

  for (const auto& e : trace.events) {
    if (e.has_flag(model::QualityFlag::kIncompleteSpan)) ++rep.incomplete_spans;
  }
  for (const auto& j : trace.js_tasks) {
    if (!j.duration_ns.has_value()) ++rep.incomplete_spans;
  }

  // Derive the capture window from every source if it was not stated. An
  // empty trace keeps a zero-length window, which rules read as "no coverage"
  // rather than "nothing happened".
  if (trace.window_start_ns == 0 && trace.window_end_ns == 0) {
    bool have = false;
    auto extend = [&](TimeNs a, TimeNs b) {
      if (!have) {
        trace.window_start_ns = a;
        trace.window_end_ns = b;
        have = true;
      } else {
        trace.window_start_ns = std::min(trace.window_start_ns, a);
        trace.window_end_ns = std::max(trace.window_end_ns, b);
      }
    };
    for (const auto& e : trace.events) {
      extend(e.timestamp, e.timestamp + e.duration_ns.value_or(0));
    }
    for (const auto& f : trace.frames) {
      extend(f.start_ns, f.presented_ns.value_or(f.start_ns));
    }
    for (const auto& j : trace.js_tasks) {
      extend(j.start_ns, j.start_ns + j.duration_ns.value_or(0));
    }
    for (const auto& s : trace.cpu_samples) extend(s.timestamp_ns, s.timestamp_ns);
    for (const auto& m : trace.markers) extend(m.timestamp_ns, m.timestamp_ns);
    if (have) {
      rep.notes.push_back("capture window derived from event extents");
    }
  }

  for (const auto& e : trace.events) {
    if (e.timestamp < trace.window_start_ns || e.timestamp > trace.window_end_ns) {
      ++rep.events_outside_window;
    }
  }
  if (rep.events_outside_window > 0) {
    rep.notes.push_back(std::to_string(rep.events_outside_window) +
                        " event(s) fall outside the declared capture window");
  }

  // A provider that reported drops turns into an explicit note; the count is
  // never quietly folded into a total.
  for (const auto& kv : trace.dropped_events_by_collector) {
    if (kv.second <= 0) continue;
    rep.notes.push_back(kv.first + " reported " + std::to_string(kv.second) +
                        " dropped event(s); affected intervals are gaps, not "
                        "zero activity");
    for (auto& e : trace.events) {
      if (e.provider == kv.first) e.add_flag(model::QualityFlag::kProviderDropped);
    }
  }

  // Any collector present in the events but missing from coverage gets a
  // derived record, so a rule can always ask "how much of the window did this
  // collector actually cover?".
  std::set<std::string> providers;
  for (const auto& e : trace.events) {
    if (!e.provider.empty()) providers.insert(e.provider);
  }
  for (const auto& p : providers) {
    if (trace.coverage_for(p) != nullptr) continue;
    trace.coverage.push_back(derive_coverage(trace, p, /*gap_threshold_ns=*/
                                             500ll * 1000 * 1000));
  }

  if (trace.partial && trace.partial_reasons.empty()) {
    trace.partial_reasons.push_back("partial flag set without a stated reason");
  }
  return rep;
}

model::Coverage derive_coverage(const model::NormalizedTrace& trace,
                                const std::string& collector,
                                TimeNs gap_threshold_ns) {
  model::Coverage cov;
  cov.collector = collector;
  cov.window_start_ns = trace.window_start_ns;
  cov.window_end_ns = trace.window_end_ns;

  std::vector<TimeNs> stamps;
  for (const auto& e : trace.events) {
    if (e.provider != collector) continue;
    stamps.push_back(e.timestamp);
  }
  cov.event_count = static_cast<std::int64_t>(stamps.size());
  if (stamps.empty()) {
    // No events at all: the entire window is a gap, not measured zero.
    model::CoverageGap g;
    g.collector = collector;
    g.start_ns = trace.window_start_ns;
    g.end_ns = trace.window_end_ns;
    g.reason = "no_events_from_collector";
    cov.gaps.push_back(std::move(g));
    return cov;
  }
  std::sort(stamps.begin(), stamps.end());

  if (stamps.front() - trace.window_start_ns > gap_threshold_ns) {
    model::CoverageGap g;
    g.collector = collector;
    g.start_ns = trace.window_start_ns;
    g.end_ns = stamps.front();
    g.reason = "collector_started_late";
    cov.gaps.push_back(std::move(g));
  }
  for (std::size_t i = 1; i < stamps.size(); ++i) {
    const TimeNs delta = stamps[i] - stamps[i - 1];
    if (delta <= gap_threshold_ns) continue;
    model::CoverageGap g;
    g.collector = collector;
    g.start_ns = stamps[i - 1];
    g.end_ns = stamps[i];
    g.reason = "no_events_in_interval";
    cov.gaps.push_back(std::move(g));
  }
  if (trace.window_end_ns - stamps.back() > gap_threshold_ns) {
    model::CoverageGap g;
    g.collector = collector;
    g.start_ns = stamps.back();
    g.end_ns = trace.window_end_ns;
    g.reason = "collector_stopped_early";
    cov.gaps.push_back(std::move(g));
  }
  return cov;
}

}  // namespace mpi::ingest
