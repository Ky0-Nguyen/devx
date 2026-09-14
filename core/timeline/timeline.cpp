#include "core/timeline/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace mpi::timeline {
namespace {

// Which coverage rows belong to a kind of measurement.
//
// A track cannot name its collector, because the collector's name is the
// provider's: Android frames come from `dumpsys gfxinfo`, iOS frames from
// `xctrace`, memory from `dumpsys meminfo` or `footprint`. Guessing one name
// per track made every bin on a real capture read `unmeasured`, which is the
// safe direction to be wrong in but still wrong. So the kind is declared and
// the names it is known by are listed here, in one place, where an added
// provider is an added string rather than a bug in a chart.
enum class SourceKind { kFrames, kCpu, kMemory, kJs, kSdk, kScheduling };

const std::vector<std::string>& providers_for(SourceKind kind) {
  static const std::vector<std::string> kFrames = {
      "frames", "dumpsys gfxinfo", "gfxinfo", "xctrace.frames",
      "core_animation"};
  static const std::vector<std::string> kCpu = {
      "cpu_samples", "simpleperf", "xctrace.time_profile", "time_profile"};
  static const std::vector<std::string> kMemory = {
      "memory", "dumpsys meminfo", "meminfo", "footprint", "xctrace.allocations"};
  static const std::vector<std::string> kJs = {"js_tasks", "hermes", "js"};
  static const std::vector<std::string> kSdk = {"sdk", "app_sdk", "markers"};
  static const std::vector<std::string> kSched = {
      "scheduling", "atrace", "ftrace"};
  switch (kind) {
    case SourceKind::kFrames: return kFrames;
    case SourceKind::kCpu: return kCpu;
    case SourceKind::kMemory: return kMemory;
    case SourceKind::kJs: return kJs;
    case SourceKind::kSdk: return kSdk;
    case SourceKind::kScheduling: return kSched;
  }
  return kFrames;
}

// Coverage lookup for one kind of measurement, over however many rows the
// capture recorded for it.
class CoverageView {
 public:
  CoverageView(const model::NormalizedTrace& trace, SourceKind kind) {
    const auto& names = providers_for(kind);
    for (const auto& c : trace.coverage) {
      const bool mine = std::any_of(
          names.begin(), names.end(),
          [&](const std::string& n) { return c.collector == n; });
      if (!mine) continue;
      matched_.push_back(c.collector);
      if (c.window_end_ns <= c.window_start_ns) {
        // A row with no window is a defect in whoever wrote it, not a
        // measurement. Treating it as coverage would claim an interval of
        // zero length covered the capture; treating it as merely absent would
        // hide that the collector ran and recorded something malformed.
        degenerate_.push_back(c.collector);
        continue;
      }
      found_ = true;
      start_ns_ = have_window_ ? std::min(start_ns_, c.window_start_ns)
                               : c.window_start_ns;
      end_ns_ = have_window_ ? std::max(end_ns_, c.window_end_ns) : c.window_end_ns;
      have_window_ = true;
      for (const auto& g : c.gaps) gaps_.push_back(&g);
    }
  }

  // Absent coverage is not the same as coverage reporting nothing. A
  // collector that never registered coverage cannot have its bins called
  // measured, because nothing says it was running.
  bool declared() const { return found_; }
  const std::vector<std::string>& matched() const { return matched_; }

  // What the bin states rest on, in words, including the case where the row
  // exists but is unusable.
  std::string note() const {
    if (!degenerate_.empty()) {
      std::string s =
          "coverage row(s) from " + join(degenerate_) +
          " declare no window (window_end is not after window_start), so "
          "nothing they cover can be established. This is a defect in the "
          "capture, not a quiet period";
      if (found_) s += "; other rows were usable";
      return s;
    }
    if (!found_) {
      return "no collector recorded a coverage window for this source, so "
             "nothing here is established as measured -- which is not the "
             "same as the source having measured nothing";
    }
    return "coverage from " + join(matched_);
  }

  // How much of [a, b) this collector covered, in nanoseconds.
  model::TimeNs covered_in(model::TimeNs a, model::TimeNs b) const {
    if (!found_ || b <= a) return 0;
    const model::TimeNs lo = std::max(a, start_ns_);
    const model::TimeNs hi = std::min(b, end_ns_);
    if (hi <= lo) return 0;
    model::TimeNs covered = hi - lo;
    for (const auto* g : gaps_) {
      const model::TimeNs glo = std::max(lo, g->start_ns);
      const model::TimeNs ghi = std::min(hi, g->end_ns);
      if (ghi > glo) covered -= (ghi - glo);
    }
    return std::max<model::TimeNs>(covered, 0);
  }

 private:
  static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (i != 0) s += ", ";
      s += "'" + v[i] + "'";
    }
    return s;
  }

  bool found_ = false;
  bool have_window_ = false;
  model::TimeNs start_ns_ = 0;
  model::TimeNs end_ns_ = 0;
  std::vector<std::string> matched_;
  std::vector<std::string> degenerate_;
  std::vector<const model::CoverageGap*> gaps_;
};

struct Grid {
  model::TimeNs start_ns = 0;
  model::TimeNs end_ns = 0;
  model::TimeNs width_ns = 0;
  int count = 0;

  model::TimeNs bin_start(int i) const {
    return start_ns + static_cast<model::TimeNs>(i) * width_ns;
  }
  // The final bin stops at the window, not a whole width past it. Letting it
  // run over made the last bin of every track partially uncovered by
  // construction, so each one ended in a '?' that described the grid's
  // arithmetic rather than anything about the capture.
  model::TimeNs bin_end(int i) const {
    return std::min(bin_start(i + 1), end_ns);
  }
  // The bin an instant falls in, or -1 when it falls outside the window.
  int index_of(model::TimeNs t) const {
    if (width_ns <= 0 || t < start_ns) return -1;
    // The window's last instant belongs to the last bin. A reading taken at
    // exactly window_end -- which is what a post-capture memory read is --
    // would otherwise be counted as outside the window it defined.
    if (t == end_ns && count > 0) return count - 1;
    const auto i = (t - start_ns) / width_ns;
    if (i >= count) return -1;
    return static_cast<int>(i);
  }
};

// Applies coverage to a track's bins. Called after the values are
// accumulated: the state decides whether a value may be shown at all, so it
// is applied last and can erase one.
void apply_coverage(Track& track, const Grid& grid, const CoverageView& cov) {
  // Whether an empty covered bin is a zero at all. Counting how many frames
  // arrived in a bin yields 0 when none did; reading how much memory the
  // process held yields nothing when it was not read. Only the first is a
  // measurement.
  const bool empty_means_zero = track.kind != TrackKind::kSeries;
  track.collectors = cov.matched();
  track.coverage_note = cov.note();
  bool any_measured = false;
  double peak = 0.0;
  for (int i = 0; i < grid.count; ++i) {
    Bin& bin = track.bins[static_cast<std::size_t>(i)];
    const model::TimeNs span = bin.end_ns - bin.start_ns;
    const model::TimeNs covered = cov.covered_in(bin.start_ns, bin.end_ns);
    if (span <= 0 || covered <= 0) {
      // Nothing covered this stretch. Any value accumulated here came from an
      // event stamped inside an uncovered window, which is a contradiction
      // worth surfacing rather than smoothing over -- but the state still
      // governs, so the value goes.
      bin.state = BinState::kUnmeasured;
      bin.value.reset();
      bin.covered_fraction = 0.0;
      continue;
    }
    if (covered >= span) {
      bin.state = BinState::kMeasured;
      bin.covered_fraction.reset();
    } else {
      bin.state = BinState::kPartial;
      bin.covered_fraction =
          static_cast<double>(covered) / static_cast<double>(span);
    }
    if (!bin.value.has_value()) {
      if (empty_means_zero) {
        // Covered, and nothing happened. This is the measured zero the state
        // exists to distinguish, and it is set explicitly rather than left
        // absent.
        bin.value = 0.0;
      } else {
        // Covered, but this quantity was not sampled here. Saying zero would
        // be inventing a reading.
        bin.state = BinState::kNoReading;
        continue;
      }
    }
    if (bin.state == BinState::kMeasured) {
      any_measured = true;
      peak = std::max(peak, *bin.value);
    }
  }
  // A peak drawn from partial bins would be a floor, not a maximum, so the
  // scale is taken from fully measured bins only -- and stays absent when
  // there are none.
  if (any_measured) track.max_value = peak;
}

Track make_track(const Grid& grid, std::string id, std::string label,
                 std::string unit, TrackKind kind) {
  Track t;
  t.id = std::move(id);
  t.label = std::move(label);
  t.unit = std::move(unit);
  t.kind = kind;
  t.bins.resize(static_cast<std::size_t>(grid.count));
  for (int i = 0; i < grid.count; ++i) {
    auto& bin = t.bins[static_cast<std::size_t>(i)];
    bin.start_ns = grid.bin_start(i);
    bin.end_ns = grid.bin_end(i);
  }
  return t;
}

void add_count(Track& track, const Grid& grid, model::TimeNs at, double weight) {
  const int i = grid.index_of(at);
  if (i < 0) {
    ++track.unplaced_events;
    return;
  }
  auto& bin = track.bins[static_cast<std::size_t>(i)];
  bin.value = bin.value.value_or(0.0) + weight;
  ++bin.event_count;
  ++track.total_events;
}

// Spreads an interval's duration across the bins it overlaps, so a 200 ms
// task is not drawn as a spike in the bin it began in.
void add_interval(Track& track, const Grid& grid, model::TimeNs start,
                  model::TimeNs end) {
  if (end < start) std::swap(start, end);
  const int first = grid.index_of(start);
  if (first < 0 && end < grid.start_ns) {
    ++track.unplaced_events;
    return;
  }
  ++track.total_events;
  bool counted = false;
  for (int i = std::max(first, 0); i < grid.count; ++i) {
    auto& bin = track.bins[static_cast<std::size_t>(i)];
    if (bin.start_ns >= end && counted) break;
    const model::TimeNs lo = std::max(bin.start_ns, start);
    const model::TimeNs hi = std::min(bin.end_ns, end);
    if (hi <= lo) {
      if (bin.start_ns >= end) break;
      continue;
    }
    bin.value = bin.value.value_or(0.0) + static_cast<double>(hi - lo);
    ++bin.event_count;
    counted = true;
  }
}

// A sampled quantity is not additive: two readings of 400 MiB in one bin do
// not make 800. The last reading in the bin is what the bin holds.
void set_series_point(Track& track, const Grid& grid, model::TimeNs at,
                      double value, model::TimeNs& last_at,
                      std::vector<model::TimeNs>& bin_last_at) {
  const int i = grid.index_of(at);
  if (i < 0) {
    ++track.unplaced_events;
    return;
  }
  auto& bin = track.bins[static_cast<std::size_t>(i)];
  auto& seen = bin_last_at[static_cast<std::size_t>(i)];
  if (bin.event_count == 0 || at >= seen) {
    bin.value = value;
    seen = at;
  }
  ++bin.event_count;
  ++track.total_events;
  last_at = at;
}

// Places an event stamped on `domain` onto the primary timeline, refusing an
// unmeasured mapping the same way the trace model does.
struct Placement {
  bool ok = false;
  model::TimeNs t = 0;
};
Placement place(const model::NormalizedTrace& trace, const std::string& domain,
                model::TimeNs t) {
  if (domain.empty() || domain == trace.primary_clock_domain) return {true, t};
  const auto mapped = trace.map_to_primary(domain, t);
  if (!mapped.has_value()) return {false, 0};
  return {true, *mapped};
}

// Whether a producer clock can be placed at all, decided once per track so a
// track that cannot be positioned is reported as such instead of silently
// dropping every event.
bool domain_is_placeable(const model::NormalizedTrace& trace,
                         const std::string& domain) {
  if (domain.empty() || domain == trace.primary_clock_domain) return true;
  return trace.map_to_primary(domain, trace.window_start_ns).has_value();
}

}  // namespace

const char* to_string(BinState s) {
  switch (s) {
    case BinState::kUnmeasured: return "unmeasured";
    case BinState::kNoReading: return "no_reading";
    case BinState::kPartial: return "partial";
    case BinState::kMeasured: return "measured";
  }
  return "unmeasured";
}

const char* to_string(TrackKind k) {
  switch (k) {
    case TrackKind::kOccurrence: return "occurrence";
    case TrackKind::kDuration: return "duration";
    case TrackKind::kSeries: return "series";
    case TrackKind::kFlag: return "flag";
  }
  return "occurrence";
}

json::Value Bin::to_json() const {
  json::Value o = json::Value::object();
  o.set("start_ns", json::Value::integer(start_ns));
  o.set("end_ns", json::Value::integer(end_ns));
  o.set("state", json::Value::string(to_string(state)));
  // Null, not 0. The whole point of the type survives into the wire format:
  // a consumer that treats null as zero has to do it deliberately.
  o.set("value", value.has_value() ? json::Value::number(*value)
                                   : json::Value::null());
  o.set("event_count", json::Value::integer(event_count));
  if (covered_fraction.has_value()) {
    o.set("covered_fraction", json::Value::number(*covered_fraction));
  }
  return o;
}

json::Value Track::to_json() const {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(id));
  o.set("label", json::Value::string(label));
  o.set("unit", json::Value::string(unit));
  o.set("kind", json::Value::string(to_string(kind)));
  o.set("provider", json::Value::string(provider));
  json::Value cols = json::Value::array();
  for (const auto& c : collectors) cols.push_back(json::Value::string(c));
  o.set("collectors", std::move(cols));
  o.set("coverage_note", json::Value::string(coverage_note));
  o.set("clock_domain", json::Value::string(clock_domain));
  o.set("placed", json::Value::boolean(placed));
  if (!placement_note.empty()) {
    o.set("placement_note", json::Value::string(placement_note));
  }
  json::Value lim = json::Value::array();
  for (const auto& l : limitations) lim.push_back(json::Value::string(l));
  o.set("limitations", std::move(lim));
  o.set("max_value", max_value.has_value() ? json::Value::number(*max_value)
                                           : json::Value::null());
  o.set("total_events", json::Value::integer(total_events));
  o.set("unplaced_events", json::Value::integer(unplaced_events));
  json::Value b = json::Value::array();
  for (const auto& bin : bins) b.push_back(bin.to_json());
  o.set("bins", std::move(b));
  return o;
}

json::Value Band::to_json() const {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(id));
  o.set("label", json::Value::string(label));
  o.set("start_ns", json::Value::integer(start_ns));
  o.set("end_ns", json::Value::integer(end_ns));
  if (!detail.empty()) o.set("detail", json::Value::string(detail));
  o.set("widened", json::Value::boolean(widened));
  return o;
}

json::Value Timeline::to_json() const {
  json::Value o = json::Value::object();
  o.set("session_id", json::Value::string(session_id));
  o.set("window_start_ns", json::Value::integer(window_start_ns));
  o.set("window_end_ns", json::Value::integer(window_end_ns));
  o.set("bin_width_ns", json::Value::integer(bin_width_ns));
  o.set("bin_count", json::Value::integer(bin_count));
  o.set("primary_clock_domain", json::Value::string(primary_clock_domain));
  if (!empty_reason.empty()) {
    o.set("empty_reason", json::Value::string(empty_reason));
  }
  json::Value t = json::Value::array();
  for (const auto& track : tracks) t.push_back(track.to_json());
  o.set("tracks", std::move(t));
  json::Value i = json::Value::array();
  for (const auto& band : issues) i.push_back(band.to_json());
  o.set("issues", std::move(i));
  json::Value g = json::Value::array();
  for (const auto& band : gaps) g.push_back(band.to_json());
  o.set("gaps", std::move(g));
  return o;
}

Timeline build(const model::NormalizedTrace& trace,
               const std::vector<model::Issue>& issues,
               const Options& opts) {
  Timeline tl;
  tl.session_id = trace.session_id;
  tl.window_start_ns = trace.window_start_ns;
  tl.window_end_ns = trace.window_end_ns;
  tl.primary_clock_domain = trace.primary_clock_domain;

  if (trace.window_end_ns <= trace.window_start_ns) {
    // No window means no axis. Saying so is the honest output; drawing an
    // empty one would imply a capture that measured nothing over a real
    // interval.
    tl.empty_reason =
        "this capture has no measured window (window_end is not after "
        "window_start), so there is no timeline to place events on";
    return tl;
  }

  Grid grid;
  grid.count = std::clamp(opts.bin_count, 1, 4096);
  grid.start_ns = trace.window_start_ns;
  grid.end_ns = trace.window_end_ns;
  const model::TimeNs duration = trace.window_end_ns - trace.window_start_ns;
  grid.width_ns = std::max<model::TimeNs>(1, duration / grid.count);
  // Integer division loses the remainder, which would leave the tail of the
  // capture outside the grid. One extra bin carries it.
  while (grid.bin_start(grid.count) < trace.window_end_ns) ++grid.count;
  tl.bin_width_ns = grid.width_ns;
  tl.bin_count = grid.count;

  // ---- frames ------------------------------------------------------------
  if (!trace.frames.empty() || true) {
    auto frames = make_track(grid, "frames", "frames", "count",
                             TrackKind::kOccurrence);
    auto missed = make_track(grid, "frames.missed", "missed deadline", "count",
                             TrackKind::kFlag);
    frames.limitations.push_back(
        "a frame count is not smoothness: the same count can be evenly paced "
        "or bunched, and this bin cannot tell them apart");
    missed.limitations.push_back(
        "only frames with both a presentation time and a platform-supplied "
        "deadline can be judged; a frame missing either is counted in the "
        "frames track and not here");
    for (const auto& f : trace.frames) {
      if (opts.cancel.cancelled()) return tl;
      if (frames.provider.empty()) {
        frames.provider = model::to_string(f.source);
        missed.provider = frames.provider;
      }
      const model::TimeNs at = f.presented_ns.value_or(f.start_ns);
      add_count(frames, grid, at, 1.0);
      const auto over = f.missed_deadline();
      if (over.has_value() && *over) add_count(missed, grid, at, 1.0);
    }
    const CoverageView cov(trace, SourceKind::kFrames);
    apply_coverage(frames, grid, cov);
    apply_coverage(missed, grid, cov);
    tl.tracks.push_back(std::move(frames));
    tl.tracks.push_back(std::move(missed));
  }

  // ---- CPU samples -------------------------------------------------------
  {
    auto samples = make_track(grid, "cpu.samples", "CPU samples", "count",
                              TrackKind::kOccurrence);
    samples.limitations.push_back(
        "sample density is not CPU load: it reflects when the sampler was "
        "running as much as what the app was doing, and a bin with no samples "
        "is not idle time");
    for (const auto& s : trace.cpu_samples) {
      if (opts.cancel.cancelled()) return tl;
      if (samples.provider.empty()) samples.provider = s.provider;
      add_count(samples, grid, s.timestamp_ns, 1.0);
    }
    apply_coverage(samples, grid, CoverageView(trace, SourceKind::kCpu));
    tl.tracks.push_back(std::move(samples));
  }

  // ---- JS tasks ----------------------------------------------------------
  {
    // Every JS task in a capture shares one clock domain in practice, but
    // nothing guarantees it, so the track is named by the domain it could
    // place and unplaceable tasks are reported rather than dropped.
    std::string domain;
    for (const auto& t : trace.js_tasks) {
      if (!t.clock_domain.empty()) { domain = t.clock_domain; break; }
    }
    auto js = make_track(grid, "js.busy", "JS thread busy", "ns",
                         TrackKind::kDuration);
    js.clock_domain = domain.empty() ? trace.primary_clock_domain : domain;
    js.limitations.push_back(
        "busy time is not blame: a long task is only a user-visible problem "
        "if it overlapped something the user was waiting for");
    if (!domain_is_placeable(trace, domain)) {
      js.placed = false;
      js.placement_note =
          "JS tasks are stamped on '" + domain +
          "', and this capture has no measured mapping from it to '" +
          trace.primary_clock_domain +
          "'. Placing them by assuming an offset would line up two unrelated "
          "clocks, so they are not placed at all";
      js.bins.clear();
      js.unplaced_events = static_cast<std::int64_t>(trace.js_tasks.size());
      tl.tracks.push_back(std::move(js));
    } else {
      for (const auto& t : trace.js_tasks) {
        if (opts.cancel.cancelled()) return tl;
        const auto p = place(trace, t.clock_domain, t.start_ns);
        if (!p.ok) { ++js.unplaced_events; continue; }
        add_interval(js, grid, p.t,
                     p.t + static_cast<model::TimeNs>(t.duration_ns.value_or(0)));
      }
      apply_coverage(js, grid, CoverageView(trace, SourceKind::kJs));
      tl.tracks.push_back(std::move(js));
    }
  }

  // ---- counters ----------------------------------------------------------
  // One track per series. Families are never merged: spec section 8 forbids
  // summing rss and pss, and a chart that stacked them would be doing exactly
  // that visually.
  for (const auto& series : trace.counters) {
    if (opts.cancel.cancelled()) return tl;
    auto track = make_track(grid, "counter." + series.name,
                            series.name.empty() ? series.family : series.name,
                            series.unit, TrackKind::kSeries);
    track.provider = series.provider;
    track.limitations.push_back(
        "each reading is an instant, not an interval: nothing is observed "
        "between points, and a bin shows its last reading rather than an "
        "average it cannot justify");
    track.limitations.push_back(
        "this family is never added to another: '" + series.family +
        "' and the other families overlap, so a total would double-count");
    std::vector<model::TimeNs> bin_last(track.bins.size(), 0);
    model::TimeNs last_at = 0;
    for (const auto& [at, v] : series.points) {
      set_series_point(track, grid, at, v, last_at, bin_last);
    }
    apply_coverage(track, grid, CoverageView(trace, SourceKind::kMemory));
    tl.tracks.push_back(std::move(track));
  }

  // ---- markers -----------------------------------------------------------
  {
    std::string domain;
    for (const auto& m : trace.markers) {
      if (!m.clock_domain.empty()) { domain = m.clock_domain; break; }
    }
    auto markers = make_track(grid, "app.markers", "app markers", "count",
                              TrackKind::kOccurrence);
    markers.provider = "app SDK";
    markers.clock_domain = domain.empty() ? trace.primary_clock_domain : domain;
    markers.limitations.push_back(
        "reported by the app about itself; the platform did not witness these");
    if (!trace.markers.empty() && !domain_is_placeable(trace, domain)) {
      markers.placed = false;
      markers.placement_note =
          "markers are stamped by the app on '" + domain +
          "' and this capture has no measured mapping to '" +
          trace.primary_clock_domain + "', so they cannot be positioned here";
      markers.bins.clear();
      markers.unplaced_events = static_cast<std::int64_t>(trace.markers.size());
    } else {
      for (const auto& m : trace.markers) {
        if (opts.cancel.cancelled()) return tl;
        const auto p = place(trace, m.clock_domain, m.timestamp_ns);
        if (!p.ok) { ++markers.unplaced_events; continue; }
        add_count(markers, grid, p.t, 1.0);
      }
      apply_coverage(markers, grid, CoverageView(trace, SourceKind::kSdk));
    }
    tl.tracks.push_back(std::move(markers));
  }

  // ---- exact bands -------------------------------------------------------
  // Gaps are drawn from the coverage records rather than inferred from empty
  // bins, because a gap the collector reported and a stretch where nothing
  // happened are different facts.
  for (const auto& c : trace.coverage) {
    for (const auto& g : c.gaps) {
      Band band;
      band.id = c.collector + ":" + std::to_string(g.start_ns);
      band.label = c.collector;
      band.start_ns = g.start_ns;
      band.end_ns = g.end_ns;
      band.detail = g.reason;
      if (g.dropped_event_count.has_value()) {
        band.detail += " (" + std::to_string(*g.dropped_event_count) +
                       " event(s) dropped)";
      }
      if (band.end_ns - band.start_ns < opts.min_band_ns) {
        band.end_ns = band.start_ns + opts.min_band_ns;
        band.widened = true;
      }
      tl.gaps.push_back(std::move(band));
    }
  }

  for (const auto& issue : issues) {
    if (issue.suppressed) continue;
    Band band;
    band.id = issue.issue_id.empty() ? issue.fingerprint : issue.issue_id;
    band.label = issue.rule_id;
    band.start_ns = issue.start_ns;
    band.end_ns = issue.end_ns;
    band.detail = issue.title;
    if (band.end_ns < band.start_ns) band.end_ns = band.start_ns;
    if (band.end_ns - band.start_ns < opts.min_band_ns) {
      // Widening is recorded because a band drawn 5 ms wide when the evidence
      // was an instant would otherwise read as a 5 ms event.
      band.end_ns = band.start_ns + opts.min_band_ns;
      band.widened = true;
    }
    tl.issues.push_back(std::move(band));
  }

  return tl;
}

}  // namespace mpi::timeline
