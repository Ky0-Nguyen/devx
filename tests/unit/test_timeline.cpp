#include <sstream>

// The timeline exists to be drawn, and drawing is where honesty gets lost:
// every lane of a chart says something about every instant it spans. These
// tests pin the specific lies a track can tell.
#include "core/timeline/timeline.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::timeline;

namespace {

// A one-second capture whose frame collector covered the whole window.
model::NormalizedTrace base_trace() {
  model::NormalizedTrace t;
  t.session_id = "tl";
  t.primary_clock_domain = "android.boottime.ns";
  t.window_start_ns = 0;
  t.window_end_ns = 1'000'000'000;
  model::Coverage cov;
  cov.collector = "frames";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  cov.event_count = 0;
  t.coverage.push_back(cov);
  return t;
}

model::FrameRecord frame_at(model::TimeNs at) {
  model::FrameRecord f;
  f.start_ns = at;
  f.presented_ns = at;
  f.source = model::FrameSource::kFrameDeadlineReports;
  return f;
}

const Track* track(const Timeline& tl, const std::string& id) {
  for (const auto& t : tl.tracks) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

Options ten_bins() {
  Options o;
  o.bin_count = 10;
  return o;
}

}  // namespace

MPI_TEST(a_covered_empty_bin_is_a_measured_zero, {"H01", "E10"}) {
  // The distinction the whole type exists for. The frame collector covered
  // the second half of this capture and the app drew nothing there: that is
  // zero, and it is allowed to be drawn as zero.
  auto t = base_trace();
  t.frames.push_back(frame_at(50'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK_EQ(frames->bins.size(), std::size_t{10});
  MPI_CHECK(frames->bins[0].state == BinState::kMeasured);
  MPI_CHECK_EQ(frames->bins[0].value.value_or(-1.0), 1.0);
  // The empty-but-covered bin: a value is present, and it is zero.
  MPI_CHECK(frames->bins[7].state == BinState::kMeasured);
  MPI_CHECK(frames->bins[7].value.has_value());
  MPI_CHECK_EQ(frames->bins[7].value.value_or(-1.0), 0.0);
}

MPI_TEST(an_uncovered_bin_has_no_value_at_all, {"H01", "D08"}) {
  // The same visual shape as above -- a flat lane -- from the opposite fact.
  // Coverage stops halfway, so the second half is unmeasured and carries no
  // number a renderer could mistake for zero.
  auto t = base_trace();
  t.coverage[0].window_end_ns = 500'000'000;
  t.frames.push_back(frame_at(50'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK(frames->bins[4].state == BinState::kMeasured);
  MPI_CHECK(frames->bins[5].state == BinState::kUnmeasured);
  MPI_CHECK_MSG(!frames->bins[5].value.has_value(),
                "an unmeasured bin must carry no value: a 0 here would be "
                "drawn as 'the app rendered nothing'");
  MPI_CHECK(!frames->bins[9].value.has_value());
}

MPI_TEST(a_gap_makes_its_bin_partial_and_says_how_much, {"D08", "D09"}) {
  // A bin with a hole in it holds a number that is wrong-low. It is shown,
  // because a count over 90% of a bin is still information, but the covered
  // fraction travels with it.
  auto t = base_trace();
  model::CoverageGap gap;
  gap.collector = "frames";
  gap.start_ns = 300'000'000;
  gap.end_ns = 350'000'000;
  gap.reason = "buffer_overrun";
  gap.dropped_event_count = 12;
  t.coverage[0].gaps.push_back(gap);
  t.frames.push_back(frame_at(310'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK(frames->bins[3].state == BinState::kPartial);
  MPI_CHECK(frames->bins[3].covered_fraction.has_value());
  MPI_CHECK_NEAR(frames->bins[3].covered_fraction.value_or(0.0), 0.5, 0.01);
  MPI_CHECK(frames->bins[2].state == BinState::kMeasured);

  // And the gap is also an exact band, from the collector's own record
  // rather than inferred from a quiet bin.
  MPI_CHECK_EQ(tl.gaps.size(), std::size_t{1});
  if (!tl.gaps.empty()) {
    MPI_CHECK_EQ(tl.gaps.front().start_ns, model::TimeNs{300'000'000});
    MPI_CHECK(tl.gaps.front().detail.find("buffer_overrun") !=
              std::string::npos);
    MPI_CHECK(tl.gaps.front().detail.find("12 event(s) dropped") !=
              std::string::npos);
  }
}

MPI_TEST(a_collector_that_never_reported_coverage_measures_nothing, {"H11"}) {
  // No coverage record for cpu_samples. That is not a capture where the CPU
  // was idle; it is a capture where nobody looked.
  const auto t = base_trace();
  const auto tl = build(t, {}, ten_bins());
  const auto* cpu = track(tl, "cpu.samples");
  MPI_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  for (const auto& bin : cpu->bins) {
    MPI_CHECK(bin.state == BinState::kUnmeasured);
    MPI_CHECK(!bin.value.has_value());
  }
  MPI_CHECK_MSG(!cpu->max_value.has_value(),
                "a track with no measured bin has no maximum");
  MPI_CHECK(!cpu->limitations.empty());
}

MPI_TEST(the_peak_ignores_partial_bins, {"D08"}) {
  // A partial bin's count is a floor. Letting it set the scale would make
  // every other bin look smaller than it was, relative to a number that was
  // never fully measured.
  auto t = base_trace();
  model::CoverageGap gap;
  gap.collector = "frames";
  gap.start_ns = 0;
  gap.end_ns = 90'000'000;
  gap.reason = "not_started";
  t.coverage[0].gaps.push_back(gap);
  // Five frames land in bin 0, which is 90% gap; two in bin 5, fully covered.
  for (int i = 0; i < 5; ++i) t.frames.push_back(frame_at(95'000'000));
  t.frames.push_back(frame_at(550'000'000));
  t.frames.push_back(frame_at(560'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK(frames->bins[0].state == BinState::kPartial);
  MPI_CHECK_EQ(frames->bins[0].value.value_or(0.0), 5.0);
  MPI_CHECK(frames->max_value.has_value());
  MPI_CHECK_MSG(frames->max_value.value_or(0.0) == 2.0,
                "the scale comes from fully measured bins only");
}

MPI_TEST(only_a_judgeable_frame_counts_as_missed, {"E21", "D11"}) {
  auto t = base_trace();
  auto missed = frame_at(100'000'000);
  // `deadline_ns` is the budget, not an instant: 8 ms allowed, 20 ms taken.
  missed.deadline_ns = 8'000'000;
  missed.presented_ns = 100'000'000 + 20'000'000;
  t.frames.push_back(missed);
  // No deadline: the platform did not say what this frame owed, so it is
  // unjudgeable rather than on time.
  t.frames.push_back(frame_at(200'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* flag = track(tl, "frames.missed");
  const auto* frames = track(tl, "frames");
  MPI_CHECK(flag != nullptr && frames != nullptr);
  if (flag == nullptr || frames == nullptr) return;
  MPI_CHECK_EQ(frames->total_events, std::int64_t{2});
  MPI_CHECK_EQ(flag->total_events, std::int64_t{1});
  MPI_CHECK(!flag->limitations.empty());
}

MPI_TEST(js_tasks_on_an_unmapped_clock_are_not_placed, {"D12", "C18"}) {
  // The refusal that matters most on a chart: an unmapped JS clock drawn
  // against device measurements would look like an alignment that was never
  // measured.
  auto t = base_trace();
  model::JsTask task;
  task.start_ns = 71'000'000;
  task.duration_ns = 40'000'000;
  task.clock_domain = "app.performance.now";
  t.js_tasks.push_back(task);
  const auto tl = build(t, {}, ten_bins());
  const auto* js = track(tl, "js.busy");
  MPI_CHECK(js != nullptr);
  if (js == nullptr) return;
  MPI_CHECK_MSG(!js->placed, "no measured mapping means no placement");
  MPI_CHECK(js->bins.empty());
  MPI_CHECK_EQ(js->unplaced_events, std::int64_t{1});
  MPI_CHECK(js->placement_note.find("app.performance.now") != std::string::npos);
  MPI_CHECK_MSG(js->placement_note.find("assuming an offset") !=
                    std::string::npos,
                "the note must say why, not just that it failed");
}

MPI_TEST(a_measured_mapping_places_them, {"D12"}) {
  // The same task, with the offset actually measured, is placed.
  auto t = base_trace();
  model::ClockMapping m;
  m.from_domain = "app.performance.now";
  m.to_domain = "android.boottime.ns";
  m.offset_ns = 500'000'000;
  m.uncertainty_ns = 11'000'000;
  m.method = "sdk handshake round-trip";
  m.measured = true;
  t.clock_mappings.push_back(m);
  model::Coverage cov;
  cov.collector = "js_tasks";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  t.coverage.push_back(cov);
  model::JsTask task;
  task.start_ns = 71'000'000;
  task.duration_ns = 40'000'000;
  task.clock_domain = "app.performance.now";
  t.js_tasks.push_back(task);
  const auto tl = build(t, {}, ten_bins());
  const auto* js = track(tl, "js.busy");
  MPI_CHECK(js != nullptr);
  if (js == nullptr) return;
  MPI_CHECK(js->placed);
  MPI_CHECK_EQ(js->unplaced_events, std::int64_t{0});
  // 571-611 ms: 29 ms in bin 5, 11 ms in bin 6.
  MPI_CHECK_NEAR(js->bins[5].value.value_or(0.0), 29'000'000.0, 1'000'000.0);
  MPI_CHECK_NEAR(js->bins[6].value.value_or(0.0), 11'000'000.0, 1'000'000.0);
  MPI_CHECK_EQ(js->bins[4].value.value_or(-1.0), 0.0);
}

MPI_TEST(an_interval_is_spread_not_spiked, {"E10"}) {
  // A 250 ms task is 250 ms of busy time, not one tall bar in the bin it
  // started in.
  auto t = base_trace();
  model::Coverage cov;
  cov.collector = "js_tasks";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  t.coverage.push_back(cov);
  model::JsTask task;
  task.start_ns = 100'000'000;
  task.duration_ns = 250'000'000;
  t.js_tasks.push_back(task);
  const auto tl = build(t, {}, ten_bins());
  const auto* js = track(tl, "js.busy");
  MPI_CHECK(js != nullptr);
  if (js == nullptr) return;
  MPI_CHECK_EQ(js->bins[1].value.value_or(0.0), 100'000'000.0);
  MPI_CHECK_EQ(js->bins[2].value.value_or(0.0), 100'000'000.0);
  MPI_CHECK_EQ(js->bins[3].value.value_or(0.0), 50'000'000.0);
  MPI_CHECK_EQ(js->bins[4].value.value_or(-1.0), 0.0);
}

MPI_TEST(a_counter_bin_holds_a_reading_not_a_sum, {"F07", "D16"}) {
  // Two readings of 400 MiB in one bin are not 800 MiB. Summing a sampled
  // quantity is the classic chart lie and the track kind forbids it.
  auto t = base_trace();
  model::Coverage cov;
  cov.collector = "memory";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  t.coverage.push_back(cov);
  model::CounterSeries rss;
  rss.name = "memory.rss_bytes";
  rss.family = "rss";
  rss.unit = "bytes";
  rss.provider = "dumpsys meminfo";
  rss.points.push_back({10'000'000, 400.0});
  rss.points.push_back({60'000'000, 410.0});
  t.counters.push_back(rss);
  const auto tl = build(t, {}, ten_bins());
  const auto* c = track(tl, "counter.memory.rss_bytes");
  MPI_CHECK(c != nullptr);
  if (c == nullptr) return;
  MPI_CHECK(c->kind == TrackKind::kSeries);
  MPI_CHECK_EQ(c->bins[0].value.value_or(0.0), 410.0);
  MPI_CHECK_EQ(c->bins[0].event_count, std::int64_t{2});
  // A bin between readings holds no reading of its own. The collector was
  // running, so this is not unmeasured -- but memory did not fall to zero
  // between samples either, so there is no value to show. An earlier version
  // filled these with 0 and drew an 879 MiB process as a flat line at the
  // bottom of the chart.
  MPI_CHECK(c->bins[5].state == BinState::kNoReading);
  MPI_CHECK_MSG(!c->bins[5].value.has_value(),
                "a covered bin with no sample must carry no value");
  bool says_instant = false;
  bool says_no_totals = false;
  for (const auto& l : c->limitations) {
    if (l.find("instant") != std::string::npos) says_instant = true;
    if (l.find("double-count") != std::string::npos) says_no_totals = true;
  }
  MPI_CHECK(says_instant);
  MPI_CHECK_MSG(says_no_totals, "families must never be presented as a total");
}

MPI_TEST(memory_families_stay_separate_tracks, {"F07"}) {
  auto t = base_trace();
  for (const char* family : {"rss", "pss", "java_heap"}) {
    model::CounterSeries s;
    s.name = std::string("memory.") + family;
    s.family = family;
    s.points.push_back({1000, 1.0});
    t.counters.push_back(s);
  }
  const auto tl = build(t, {}, ten_bins());
  int counter_tracks = 0;
  for (const auto& tr : tl.tracks) {
    if (tr.kind == TrackKind::kSeries) ++counter_tracks;
  }
  MPI_CHECK_MSG(counter_tracks == 3,
                "one track per family: a stacked chart would be a sum");
}

MPI_TEST(an_instant_issue_band_is_widened_and_says_so, {"H01"}) {
  auto t = base_trace();
  model::Issue issue;
  issue.issue_id = "i1";
  issue.rule_id = "DET-01";
  issue.title = "one late frame";
  issue.start_ns = 400'000'000;
  issue.end_ns = 400'000'000;
  Options o = ten_bins();
  o.min_band_ns = 5'000'000;
  const auto tl = build(t, {issue}, o);
  MPI_CHECK_EQ(tl.issues.size(), std::size_t{1});
  if (tl.issues.empty()) return;
  MPI_CHECK(tl.issues.front().widened);
  MPI_CHECK_EQ(tl.issues.front().end_ns - tl.issues.front().start_ns,
               model::TimeNs{5'000'000});
  MPI_CHECK_EQ(tl.issues.front().start_ns, model::TimeNs{400'000'000});
}

MPI_TEST(a_real_interval_is_never_widened, {"H01"}) {
  auto t = base_trace();
  model::Issue issue;
  issue.issue_id = "i1";
  issue.rule_id = "DET-02";
  issue.start_ns = 100'000'000;
  issue.end_ns = 180'000'000;
  Options o = ten_bins();
  o.min_band_ns = 5'000'000;
  const auto tl = build(t, {issue}, o);
  MPI_CHECK_EQ(tl.issues.size(), std::size_t{1});
  if (tl.issues.empty()) return;
  MPI_CHECK(!tl.issues.front().widened);
  MPI_CHECK_EQ(tl.issues.front().end_ns, model::TimeNs{180'000'000});
}

MPI_TEST(a_suppressed_issue_is_not_drawn, {"H14"}) {
  auto t = base_trace();
  model::Issue issue;
  issue.issue_id = "i1";
  issue.rule_id = "DET-01";
  issue.start_ns = 1000;
  issue.end_ns = 2000;
  issue.suppressed = true;
  issue.suppression_reason = "known third-party";
  const auto tl = build(t, {issue}, ten_bins());
  MPI_CHECK_MSG(tl.issues.empty(),
                "a suppressed issue is not on the timeline; the suppression "
                "record is where it stays visible");
}

MPI_TEST(a_capture_with_no_window_yields_no_axis, {"H01", "D08"}) {
  model::NormalizedTrace t;
  t.session_id = "empty";
  t.primary_clock_domain = "android.boottime.ns";
  const auto tl = build(t, {}, ten_bins());
  MPI_CHECK(tl.tracks.empty());
  MPI_CHECK(!tl.empty_reason.empty());
  MPI_CHECK_MSG(tl.empty_reason.find("no measured window") != std::string::npos,
                "an empty axis would imply a capture that measured nothing "
                "over a real interval");
}

MPI_TEST(the_bin_count_does_not_follow_the_capture_size, {"I21"}) {
  // The 1 GiB stress fixture and a 1 MiB capture must cost the UI the same.
  auto t = base_trace();
  t.window_end_ns = 600'000'000'000;  // ten minutes
  t.coverage[0].window_end_ns = t.window_end_ns;
  for (int i = 0; i < 50'000; ++i) {
    t.frames.push_back(frame_at(static_cast<model::TimeNs>(i) * 12'000'000));
  }
  Options o;
  o.bin_count = 240;
  const auto tl = build(t, {}, o);
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK(frames->bins.size() <= 242);
  MPI_CHECK_EQ(frames->total_events, std::int64_t{50'000});
  // The window's tail is inside the grid: integer division would otherwise
  // leave the last fraction of a bin unrepresented.
  MPI_CHECK(frames->bins.back().end_ns >= t.window_end_ns);
}

MPI_TEST(an_event_outside_the_window_is_counted_as_unplaced, {"D10"}) {
  auto t = base_trace();
  t.frames.push_back(frame_at(50'000'000));
  t.frames.push_back(frame_at(9'000'000'000));  // long after the window
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK_EQ(frames->total_events, std::int64_t{1});
  MPI_CHECK_MSG(frames->unplaced_events == 1,
                "an event outside the window is reported, not discarded");
}

MPI_TEST(cancellation_stops_the_build, {"J11"}) {
  auto t = base_trace();
  for (int i = 0; i < 1000; ++i) {
    t.frames.push_back(frame_at(static_cast<model::TimeNs>(i) * 1'000'000));
  }
  CancellationSource src;
  Options o = ten_bins();
  o.cancel = src.token();
  src.cancel();
  const auto tl = build(t, {}, o);
  MPI_CHECK_MSG(tl.tracks.empty(),
                "a cancelled build returns what it had, not a full timeline");
}

MPI_TEST(json_writes_an_unmeasured_bin_as_null, {"H01", "F16"}) {
  // The wire format has to carry the distinction too: a consumer that turns
  // null into 0 has to choose to.
  auto t = base_trace();
  t.coverage[0].window_end_ns = 500'000'000;
  t.frames.push_back(frame_at(50'000'000));
  const auto tl = build(t, {}, ten_bins());
  const std::string text = tl.to_json().dump();
  MPI_CHECK(text.find("\"state\":\"unmeasured\"") != std::string::npos);
  MPI_CHECK(text.find("\"value\":null") != std::string::npos);
  MPI_CHECK(text.find("\"state\":\"measured\"") != std::string::npos);
}

MPI_TEST(a_covered_bin_without_a_sample_is_not_a_zero, {"H01", "F07"}) {
  // The counterpart to `a_covered_empty_bin_is_a_measured_zero`: whether an
  // empty covered bin means zero depends entirely on what the track measures.
  // Zero frames arrived is a measurement. Zero bytes held is not.
  auto t = base_trace();
  model::Coverage cov;
  cov.collector = "dumpsys meminfo";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  t.coverage.push_back(cov);
  model::CounterSeries rss;
  rss.name = "memory.rss_bytes";
  rss.family = "rss";
  rss.unit = "bytes";
  rss.points.push_back({950'000'000, 921'000'000.0});  // one reading, at the end
  t.counters.push_back(rss);
  t.frames.push_back(frame_at(50'000'000));
  const auto tl = build(t, {}, ten_bins());

  const auto* mem = track(tl, "counter.memory.rss_bytes");
  const auto* frames = track(tl, "frames");
  MPI_CHECK(mem != nullptr && frames != nullptr);
  if (mem == nullptr || frames == nullptr) return;
  // Same bin index, same coverage, opposite meanings.
  MPI_CHECK(frames->bins[3].state == BinState::kMeasured);
  MPI_CHECK_EQ(frames->bins[3].value.value_or(-1.0), 0.0);
  MPI_CHECK(mem->bins[3].state == BinState::kNoReading);
  MPI_CHECK(!mem->bins[3].value.has_value());
  // And the one real reading is present, so the peak is the reading rather
  // than a zero invented by the bins around it.
  MPI_CHECK(mem->bins[9].state == BinState::kMeasured);
  MPI_CHECK_EQ(mem->max_value.value_or(0.0), 921'000'000.0);
}

MPI_TEST(the_last_bin_stops_at_the_window, {"D08"}) {
  // The grid's own arithmetic must not manufacture a partial bin: a
  // remainder bin running a full width past window_end made the final column
  // of every track read as partially covered, which described the division
  // rather than the capture.
  auto t = base_trace();
  t.window_end_ns = 1'000'000'007;  // a window that does not divide evenly
  t.coverage[0].window_end_ns = t.window_end_ns;
  t.frames.push_back(frame_at(500'000'000));
  const auto tl = build(t, {}, ten_bins());
  const auto* frames = track(tl, "frames");
  MPI_CHECK(frames != nullptr);
  if (frames == nullptr) return;
  MPI_CHECK_EQ(frames->bins.back().end_ns, t.window_end_ns);
  for (const auto& bin : frames->bins) {
    MPI_CHECK_MSG(bin.state != BinState::kPartial,
                  "no bin is partial: the collector covered the whole window");
  }
}

MPI_TEST(a_reading_at_the_windows_last_instant_is_inside_it, {"D10", "F07"}) {
  // A batch capture reads memory once, after the sources it timed, so the
  // reading lands on window_end exactly -- the instant it defined. Treating
  // that as out of range dropped the only memory measurement in the capture.
  auto t = base_trace();
  model::Coverage cov;
  cov.collector = "dumpsys meminfo";
  cov.window_start_ns = 0;
  cov.window_end_ns = 1'000'000'000;
  t.coverage.push_back(cov);
  model::CounterSeries rss;
  rss.name = "memory.rss_bytes";
  rss.family = "rss";
  rss.points.push_back({1'000'000'000, 878.0});
  t.counters.push_back(rss);
  const auto tl = build(t, {}, ten_bins());
  const auto* mem = track(tl, "counter.memory.rss_bytes");
  MPI_CHECK(mem != nullptr);
  if (mem == nullptr) return;
  MPI_CHECK_EQ(mem->unplaced_events, std::int64_t{0});
  MPI_CHECK_EQ(mem->bins.back().value.value_or(0.0), 878.0);
}
