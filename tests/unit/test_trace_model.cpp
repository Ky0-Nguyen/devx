#include <sstream>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::model;

MPI_TEST(deadline_comes_from_observed_refresh_rate, {"E01"}) {
  // There is no universal 16 ms rule: each rate yields its own deadline.
  RefreshInterval r60{0, 1000, 60.0, false, "t"};
  RefreshInterval r90{0, 1000, 90.0, false, "t"};
  RefreshInterval r120{0, 1000, 120.0, false, "t"};
  MPI_CHECK(r60.deadline_ns().has_value());
  MPI_CHECK_NEAR(*r60.deadline_ns(), 16666666, 2);
  MPI_CHECK_NEAR(*r90.deadline_ns(), 11111111, 2);
  MPI_CHECK_NEAR(*r120.deadline_ns(), 8333333, 2);
}

MPI_TEST(variable_refresh_yields_no_single_deadline, {"E01", "E02"}) {
  RefreshInterval v{0, 1000, 120.0, true, "t"};
  MPI_CHECK_MSG(!v.deadline_ns().has_value(),
                "a variable-rate window has no defensible single deadline");
  RefreshInterval unknown{0, 1000, std::nullopt, false, "t"};
  MPI_CHECK(!unknown.deadline_ns().has_value());
}

MPI_TEST(refresh_rate_change_mid_session_selects_the_right_interval, {"E02"}) {
  NormalizedTrace t;
  t.refresh_intervals.push_back(RefreshInterval{0, 1000, 60.0, false, "t"});
  t.refresh_intervals.push_back(RefreshInterval{1000, 2000, 120.0, false, "t"});
  MPI_CHECK(t.refresh_at(500) != nullptr);
  MPI_CHECK_NEAR(*t.refresh_at(500)->deadline_ns(), 16666666, 2);
  MPI_CHECK_NEAR(*t.refresh_at(1500)->deadline_ns(), 8333333, 2);
  MPI_CHECK_MSG(t.refresh_at(5000) == nullptr,
                "outside every observed interval the rate is unknown");
}

MPI_TEST(frame_without_presentation_cannot_be_judged, {"E21", "D11"}) {
  FrameRecord f;
  f.start_ns = 0;
  f.deadline_ns = 16666666;
  f.presented_ns = std::nullopt;
  MPI_CHECK_MSG(!f.missed_deadline().has_value(),
                "no presentation timestamp means the question is unanswerable");
  MPI_CHECK(!f.overrun_ns().has_value());
  MPI_CHECK(f.to_json().find("missed_deadline")->is_null());
}

MPI_TEST(frame_without_deadline_cannot_be_judged, {"E01"}) {
  FrameRecord f;
  f.start_ns = 0;
  f.presented_ns = 30000000;
  f.deadline_ns = std::nullopt;
  MPI_CHECK(!f.missed_deadline().has_value());
}

MPI_TEST(proxy_frame_source_is_not_presentation_truth, {"E21"}) {
  FrameRecord f;
  f.source = FrameSource::kDisplayCallbackProxy;
  MPI_CHECK_EQ(f.to_json().find("is_presentation_truth")->as_bool(), false);
  f.source = FrameSource::kFrameDeadlineReports;
  MPI_CHECK_EQ(f.to_json().find("is_presentation_truth")->as_bool(), false);
  f.source = FrameSource::kPresentationTimestamps;
  MPI_CHECK_EQ(f.to_json().find("is_presentation_truth")->as_bool(), true);
}

MPI_TEST(unmapped_clock_domain_is_not_silently_aligned, {"D12", "section-6"}) {
  NormalizedTrace t;
  t.primary_clock_domain = "ui";
  ClockMapping assumed;
  assumed.from_domain = "js";
  assumed.to_domain = "ui";
  assumed.offset_ns = 5000;
  assumed.measured = false;  // assumed, not measured
  t.clock_mappings.push_back(assumed);
  MPI_CHECK_MSG(!t.map_to_primary("js", 1000).has_value(),
                "an unmeasured mapping must not be applied");

  t.clock_mappings[0].measured = true;
  MPI_CHECK(t.map_to_primary("js", 1000).has_value());
  MPI_CHECK_EQ(*t.map_to_primary("js", 1000), static_cast<TimeNs>(6000));

  // A domain with no mapping at all stays unmappable.
  MPI_CHECK(!t.map_to_primary("gpu", 1000).has_value());
  // The primary domain maps to itself.
  MPI_CHECK_EQ(*t.map_to_primary("ui", 42), static_cast<TimeNs>(42));
}

MPI_TEST(coverage_gap_is_not_measured_zero, {"D09", "E13"}) {
  Coverage c;
  c.collector = "sampler";
  c.window_start_ns = 0;
  c.window_end_ns = 1000;
  c.event_count = 3;
  CoverageGap g;
  g.start_ns = 400;
  g.end_ns = 900;
  g.reason = "collector_crash";
  c.gaps.push_back(g);
  MPI_CHECK_EQ(c.covered_ns(), static_cast<TimeNs>(500));
  MPI_CHECK_NEAR(c.covered_fraction(), 0.5, 1e-9);
  // The gap is a first-class object in the export, not an absence.
  MPI_CHECK_EQ(c.to_json().find("gaps")->size(), static_cast<std::size_t>(1));
}

MPI_TEST(coverage_clamps_gaps_to_the_window, {"D09"}) {
  Coverage c;
  c.window_start_ns = 100;
  c.window_end_ns = 200;
  CoverageGap g;
  g.start_ns = 0;      // starts before the window
  g.end_ns = 1000;     // ends after it
  c.gaps.push_back(g);
  MPI_CHECK_EQ(c.covered_ns(), static_cast<TimeNs>(0));
  MPI_CHECK_NEAR(c.covered_fraction(), 0.0, 1e-9);
}

MPI_TEST(unknown_metric_value_serializes_as_null_not_zero, {"section-8"}) {
  Metric m;
  m.name = "cpu.time_ns";
  m.unit = "ns";
  m.value = std::nullopt;
  MPI_CHECK(!m.measured());
  const auto j = m.to_json();
  MPI_CHECK_MSG(j.find("value")->is_null(),
                "a metric that was not collected must not read as 0");
  MPI_CHECK_EQ(j.find("measured")->as_bool(), false);
}

MPI_TEST(cpu_percentage_declares_its_normalization, {"E12"}) {
  Metric m;
  m.cpu_normalization = CpuNormalization::kUnknown;
  MPI_CHECK_EQ(m.to_json().find("cpu_normalization")->as_string(),
               std::string("unknown"));
  m.cpu_normalization = CpuNormalization::kAllCores;
  MPI_CHECK_EQ(m.to_json().find("cpu_normalization")->as_string(),
               std::string("all_cores"));
}

MPI_TEST(attribution_forbids_subtraction_structurally, {"F14", "section-9"}) {
  AttributionReport r;
  r.original_total.name = "rss_bytes";
  r.original_total.value = 280.0 * 1024 * 1024;
  AttributedSlice s;
  s.category = AttributionCategory::kProfiler;
  s.label = "recorder_buffers";
  s.value = 8.0 * 1024 * 1024;
  r.slices.push_back(s);
  const auto j = r.to_json();
  MPI_CHECK_EQ(j.find("subtraction_to_estimate_release_permitted")->as_bool(),
               false);
  // The original total is always preserved alongside the slices.
  MPI_CHECK(!j.find("original_total")->find("value")->is_null());
}

MPI_TEST(inclusive_share_is_not_summable_as_disjoint_cost, {"F17"}) {
  CandidateStack inclusive;
  inclusive.inclusive = true;
  inclusive.sample_share = 0.6;
  MPI_CHECK_EQ(inclusive.to_json().find("summable_as_disjoint_cost")->as_bool(),
               false);
  CandidateStack self_only;
  self_only.inclusive = false;
  MPI_CHECK_EQ(self_only.to_json().find("summable_as_disjoint_cost")->as_bool(),
               true);
}

MPI_TEST(non_exact_symbol_match_is_not_safe_to_open, {"G16", "G18", "G19"}) {
  SourceLocation l;
  for (const char* status : {"partial", "unresolved", "mismatch", "unavailable"}) {
    l.symbol_status = status;
    MPI_CHECK_MSG(!l.safe_to_open(),
                  std::string("must not navigate on status ") + status);
    MPI_CHECK_EQ(l.to_json().find("safe_to_open")->as_bool(), false);
  }
  l.symbol_status = "exact_build_match";
  MPI_CHECK(l.safe_to_open());
}

MPI_TEST(screen_is_null_when_not_observed, {"section-11"}) {
  Issue i;
  i.screen = "";
  MPI_CHECK_MSG(i.to_json().find("screen")->is_null(),
                "a screen must never be guessed from a function name");
  i.screen = "CartScreen";
  MPI_CHECK_EQ(i.to_json().find("screen")->as_string(), std::string("CartScreen"));
}

MPI_TEST(rule_outcome_distinguishes_skipped_from_found_nothing, {"H05", "H11"}) {
  RuleRunRecord skipped;
  skipped.outcome = RuleOutcome::kSkipped;
  skipped.skipped_reasons.push_back("no frame collector");
  RuleRunRecord nothing;
  nothing.outcome = RuleOutcome::kRanFoundNothing;
  MPI_CHECK_EQ(skipped.to_json().find("outcome")->as_string(),
               std::string("skipped"));
  MPI_CHECK_EQ(nothing.to_json().find("outcome")->as_string(),
               std::string("ran_found_nothing"));
  MPI_CHECK(skipped.to_json().find("skipped_reasons")->size() > 0);
}
