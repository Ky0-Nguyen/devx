#include <fstream>

#include <sstream>

#include "core/rules/engine.hpp"
#include "core/session/compare.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::session;

namespace {

RunConditions good_conditions(const char* platform = "android") {
  RunConditions c;
  c.platform = platform;
  c.scenario_id = "checkout";
  c.scenario_version = "3";
  c.device_id = "serialX";
  c.device_model = "Pixel 8";
  c.device_form = "physical";
  c.os_version = "14";
  c.refresh_policy = "fixed-120";
  c.collector_preset = "lightweight";
  c.collector_sample_rate_hz = 1000.0;
  c.launch_class = "cold";
  c.thermal_state = "nominal";
  c.power_state = "battery";
  c.input_data_version = "seed-7";
  c.account_state = "logged-in-fixture";
  c.network_condition = "wifi-stable";
  c.cache_state = "warm";
  return c;
}

model::Eligibility eligible() {
  model::Eligibility e;
  e.status = model::EligibilityStatus::kEligible;
  return e;
}

RunSet make_set(const char* label, const std::vector<double>& values,
                const char* platform = "android") {
  RunSet s;
  s.label = label;
  s.conditions = good_conditions(platform);
  s.mode = model::MeasurementMode::kBenchmark;
  s.eligibility = eligible();
  for (std::size_t i = 0; i < values.size(); ++i) {
    RunMeasurement m;
    m.session_id = std::string(label) + "-" + std::to_string(i);
    m.metric_name = "startup.first_frame_ns";
    m.unit = "ns";
    m.value = values[i];
    m.scenario_completed = true;
    s.runs.push_back(std::move(m));
  }
  return s;
}

ComparisonThresholds default_thresholds() {
  ComparisonThresholds t;
  t.min_valid_runs = 5;
  t.min_relative_delta = 0.05;
  t.min_absolute_delta = 1000000.0;  // 1 ms
  t.max_relative_spread = 0.25;
  return t;
}

bool mentions(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v) {
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(clear_regression_is_detected, {"I01", "I13"}) {
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         make_set("candidate", {600e6, 605e6, 598e6, 602e6, 601e6}),
                         default_thresholds());
  MPI_CHECK_MSG(r.incompatibilities.empty(),
                r.incompatibilities.empty() ? "" : r.incompatibilities[0]);
  MPI_CHECK(r.overall == ComparisonVerdict::kRegression);
  MPI_CHECK_EQ(r.metrics.size(), static_cast<std::size_t>(1));
  MPI_CHECK(r.metrics[0].verdict == ComparisonVerdict::kRegression);
  MPI_CHECK(r.metrics[0].certified);
  MPI_CHECK_NEAR(*r.metrics[0].relative_delta, 0.5025, 0.01);
}

MPI_TEST(clear_improvement_is_detected, {"I13"}) {
  const auto r = compare(make_set("baseline", {600e6, 605e6, 598e6, 602e6, 601e6}),
                         make_set("candidate", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kImprovement);
}

MPI_TEST(small_change_is_not_significant, {"I13"}) {
  const auto r = compare(make_set("baseline", {400e6, 401e6, 399e6, 400e6, 400e6}),
                         make_set("candidate", {401e6, 402e6, 400e6, 401e6, 401e6}),
                         default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kNoSignificantChange);
}

MPI_TEST(both_absolute_and_relative_thresholds_must_clear, {"I13"}) {
  // A 20% relative change that is only 0.2 ms in absolute terms.
  auto t = default_thresholds();
  t.min_absolute_delta = 1000000.0;  // 1 ms
  const auto r = compare(make_set("baseline", {1e6, 1e6, 1e6, 1e6, 1e6}),
                         make_set("candidate", {1.2e6, 1.2e6, 1.2e6, 1.2e6, 1.2e6}),
                         t);
  MPI_CHECK_MSG(r.overall == ComparisonVerdict::kNoSignificantChange,
                "a large relative change below the absolute floor is not a "
                "regression");
  MPI_CHECK(mentions(r.metrics[0].reasons, "absolute delta below"));
}

MPI_TEST(too_few_runs_is_inconclusive, {"I09"}) {
  const auto r = compare(make_set("baseline", {400e6, 402e6}),
                         make_set("candidate", {800e6, 805e6}),
                         default_thresholds());
  MPI_CHECK_MSG(r.overall == ComparisonVerdict::kInconclusive,
                "two runs cannot support a regression verdict");
  MPI_CHECK(mentions(r.metrics[0].reasons, "too few valid runs"));
}

MPI_TEST(high_variance_is_inconclusive, {"I10"}) {
  const auto r = compare(
      make_set("baseline", {100e6, 900e6, 200e6, 800e6, 300e6, 700e6}),
      make_set("candidate", {150e6, 950e6, 250e6, 850e6, 350e6, 750e6}),
      default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.metrics[0].reasons, "spread"));
  // The spread itself is reported, not hidden.
  MPI_CHECK(r.metrics[0].baseline_spread.has_value());
}

MPI_TEST(zero_baseline_avoids_division, {"I12"}) {
  const auto r = compare(make_set("baseline", {0, 0, 0, 0, 0}),
                         make_set("candidate", {5e6, 5e6, 5e6, 5e6, 5e6}),
                         default_thresholds());
  MPI_CHECK_MSG(!r.metrics[0].relative_delta.has_value(),
                "no relative change is defined against a zero baseline");
  MPI_CHECK(r.metrics[0].absolute_delta.has_value());
  MPI_CHECK(mentions(r.metrics[0].reasons, "baseline median is zero"));
  // And the JSON keeps it null rather than emitting inf.
  MPI_CHECK(r.metrics[0].to_json().find("relative_delta")->is_null());
}

MPI_TEST(device_model_mismatch_blocks_a_verdict, {"I02"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.device_model = "Pixel 6";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "device_model differs"));
}

MPI_TEST(simulator_vs_physical_is_never_comparable, {"J18", "I02"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.device_form = "simulator";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "not interchangeable"));
}

MPI_TEST(os_and_refresh_mismatch_blocks_a_verdict, {"I03"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.os_version = "15";
  candidate.conditions.refresh_policy = "fixed-60";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "os_version differs"));
  MPI_CHECK(mentions(r.incompatibilities, "refresh_policy differs"));
}

MPI_TEST(thermal_and_power_mismatch_is_visible, {"I04", "I05"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.thermal_state = "serious";
  candidate.conditions.power_state = "charging";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(mentions(r.incompatibilities, "thermal_state differs"));
  MPI_CHECK(mentions(r.incompatibilities, "power_state differs"));
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
}

MPI_TEST(launch_class_mismatch_blocks_a_verdict, {"I07"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.launch_class = "warm";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(mentions(r.incompatibilities, "launch_class differs"));
}

MPI_TEST(cache_network_and_input_mismatch_blocks_a_verdict, {"I08"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.cache_state = "cold";
  candidate.conditions.network_condition = "3g-throttled";
  candidate.conditions.input_data_version = "seed-9";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(mentions(r.incompatibilities, "cache_state differs"));
  MPI_CHECK(mentions(r.incompatibilities, "network_condition differs"));
  MPI_CHECK(mentions(r.incompatibilities, "input_data_version differs"));
}

MPI_TEST(collector_sample_rate_mismatch_blocks_a_verdict, {"I17"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.conditions.collector_sample_rate_hz = 250.0;
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(mentions(r.incompatibilities, "sample rate differs"));
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
}

MPI_TEST(debug_versus_release_is_not_a_certified_comparison, {"I16", "F14"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.mode = model::MeasurementMode::kDiagnostic;
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "cannot certify"));
}

MPI_TEST(ineligible_side_blocks_certification, {"C19"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.eligibility.status = model::EligibilityStatus::kIneligible;
  candidate.eligibility.reasons.push_back("js_dev_mode_enabled");
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "js_dev_mode_enabled"));
  MPI_CHECK(!r.metrics[0].certified);
}

MPI_TEST(cross_platform_pair_cannot_gate, {"I19", "E22"}) {
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}, "android"),
                         make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6}, "ios"),
                         default_thresholds());
  MPI_CHECK(r.cross_platform);
  MPI_CHECK_MSG(r.overall == ComparisonVerdict::kInconclusive,
                "a cross-platform pair must never yield a regression verdict");
  MPI_CHECK(mentions(r.incompatibilities, "metric semantics"));
  MPI_CHECK_EQ(r.to_json().find("usable_as_regression_gate")->as_bool(), false);
}

MPI_TEST(unknown_platform_blocks_equivalence, {"I19"}) {
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}, ""),
                         make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6}, "ios"),
                         default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
  MPI_CHECK(mentions(r.incompatibilities, "platform is unknown"));
}

MPI_TEST(incomplete_scenario_is_excluded_not_counted_as_fast, {"I15"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  RunMeasurement crashed;
  crashed.session_id = "crashed-run";
  crashed.metric_name = "startup.first_frame_ns";
  crashed.unit = "ns";
  crashed.value = 1.0;  // would look like an impossibly fast run
  crashed.scenario_completed = false;
  candidate.runs.push_back(crashed);

  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK_EQ(r.metrics[0].candidate_valid_runs, static_cast<std::size_t>(5));
  MPI_CHECK(mentions(r.metrics[0].excluded_runs, "not a fast run"));
  // The 1 ns run must not have dragged the median down.
  MPI_CHECK(*r.metrics[0].candidate_median > 700e6);
}

MPI_TEST(warm_up_runs_are_excluded_and_visible, {"section-12"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  RunMeasurement warm;
  warm.session_id = "warmup-0";
  warm.metric_name = "startup.first_frame_ns";
  warm.unit = "ns";
  warm.value = 2000e6;
  warm.scenario_completed = true;
  warm.warm_up = true;
  candidate.runs.insert(candidate.runs.begin(), warm);

  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK_EQ(r.metrics[0].candidate_valid_runs, static_cast<std::size_t>(5));
  MPI_CHECK(mentions(r.metrics[0].excluded_runs, "warm-up run"));
}

MPI_TEST(run_with_no_measurement_is_excluded_and_visible, {"I11", "I14"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  RunMeasurement nodata;
  nodata.session_id = "nodata-0";
  nodata.metric_name = "startup.first_frame_ns";
  nodata.unit = "ns";
  nodata.value = std::nullopt;
  nodata.scenario_completed = true;
  candidate.runs.push_back(nodata);
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK(mentions(r.metrics[0].excluded_runs, "no measurement produced"));
}

MPI_TEST(missing_baseline_is_inconclusive_not_a_pass, {"I11"}) {
  RunSet empty;
  empty.label = "baseline";
  empty.conditions = good_conditions();
  empty.mode = model::MeasurementMode::kBenchmark;
  empty.eligibility = eligible();
  const auto r = compare(std::move(empty),
                         make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6}),
                         default_thresholds());
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
}

MPI_TEST(explicit_exclusion_reason_is_honoured_and_shown, {"I14"}) {
  auto candidate = make_set("candidate", {800e6, 805e6, 798e6, 802e6, 801e6});
  candidate.runs[0].exclusion_reason = "background OS update detected";
  const auto r = compare(make_set("baseline", {400e6, 402e6, 398e6, 401e6, 399e6}),
                         std::move(candidate), default_thresholds());
  MPI_CHECK_EQ(r.metrics[0].candidate_valid_runs, static_cast<std::size_t>(4));
  MPI_CHECK(mentions(r.metrics[0].excluded_runs, "background OS update"));
  // Four runs is below the minimum, so the verdict drops to inconclusive.
  MPI_CHECK(r.overall == ComparisonVerdict::kInconclusive);
}

MPI_TEST(median_and_spread_are_both_reported, {"section-12"}) {
  const auto r = compare(make_set("baseline", {400e6, 410e6, 420e6, 430e6, 440e6, 450e6}),
                         make_set("candidate", {600e6, 610e6, 620e6, 630e6, 640e6, 650e6}),
                         default_thresholds());
  MPI_CHECK(r.metrics[0].baseline_median.has_value());
  MPI_CHECK(r.metrics[0].candidate_median.has_value());
  MPI_CHECK(r.metrics[0].baseline_spread.has_value());
  MPI_CHECK(r.metrics[0].candidate_spread.has_value());
  MPI_CHECK_NEAR(*r.metrics[0].baseline_median, 425e6, 1e6);
}


// --- DET-08, which renders a comparison as an issue --------------------------

namespace {

model::AnalysisResult detect(const ComparisonResult& cmp) {
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kBenchmark;
  return rules::analyze_comparison(to_regression_input(cmp), opts);
}

const model::RuleRunRecord* det08_record(const model::AnalysisResult& r) {
  for (const auto& rec : r.rule_runs) {
    if (rec.rule_id == "DET-08") return &rec;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(det08_reports_a_regression_as_an_issue, {"DET-08", "I13", "H01"}) {
  const auto cmp = compare(make_set("baseline", {100e6, 101e6, 99e6, 100e6, 102e6}),
                           make_set("candidate", {130e6, 132e6, 129e6, 131e6, 130e6}),
                           default_thresholds());
  MPI_CHECK(cmp.overall == ComparisonVerdict::kRegression);

  const auto analysis = detect(cmp);
  MPI_CHECK_EQ(analysis.issues.size(), std::size_t{1});
  const auto& issue = analysis.issues.front();
  MPI_CHECK_EQ(issue.rule_id, std::string("DET-08"));
  // The difference is measured, so detection is observed -- but a regression
  // never explains itself, so the cause stays unknown.
  MPI_CHECK(issue.detection_status == model::DetectionStatus::kObserved);
  MPI_CHECK(issue.cause_status == model::CauseStatus::kUnknown);
  MPI_CHECK(mentions(issue.missing_evidence, "no profile"));
  MPI_CHECK(!issue.threshold_expression.empty());
  MPI_CHECK(!issue.severity_rationale.empty());
  // Both medians and the delta travel with the issue.
  MPI_CHECK_EQ(issue.metrics.size(), std::size_t{3});
  // Two run sets share no timeline, so no interval is invented.
  MPI_CHECK_EQ(issue.start_ns, model::TimeNs{0});
  MPI_CHECK_EQ(issue.end_ns, model::TimeNs{0});
}

MPI_TEST(det08_refuses_a_debug_versus_release_pair, {"DET-08", "I16"}) {
  auto candidate = make_set("candidate", {130e6, 132e6, 129e6, 131e6, 130e6});
  candidate.mode = model::MeasurementMode::kDiagnostic;
  model::Eligibility ineligible;
  ineligible.status = model::EligibilityStatus::kIneligible;
  ineligible.reasons.push_back("mode_is_not_benchmark:diagnostic");
  candidate.eligibility = ineligible;

  const auto analysis = detect(compare(
      make_set("baseline", {100e6, 101e6, 99e6, 100e6, 102e6}), std::move(candidate),
      default_thresholds()));
  // The comparison refuses the gate outright, so there is no regression issue
  // to qualify -- and the reason names eligibility rather than the numbers.
  MPI_CHECK(analysis.issues.empty());
  const auto* rec = det08_record(analysis);
  MPI_CHECK(rec != nullptr);
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK(mentions(rec->skipped_reasons, "not benchmark-eligible"));
}

MPI_TEST(det08_says_a_forced_pair_certifies_nothing, {"DET-08", "I16", "C19"}) {
  // A user-forced comparison is the one route to a regression verdict whose
  // sides are not certified: the override is recorded, and it does not erase
  // the invalidity. The issue has to carry that, or the number reads as a
  // release regression.
  model::Eligibility forced;
  forced.status = model::EligibilityStatus::kEligible;
  forced.user_override = true;
  forced.user_override_note = "exploratory run, forced by the operator";

  auto baseline = make_set("baseline", {100e6, 101e6, 99e6, 100e6, 102e6});
  auto candidate = make_set("candidate", {130e6, 132e6, 129e6, 131e6, 130e6});
  baseline.eligibility = forced;
  candidate.eligibility = forced;

  const auto cmp = compare(std::move(baseline), std::move(candidate),
                           default_thresholds());
  MPI_CHECK(cmp.metrics.size() == 1);
  MPI_CHECK(!cmp.metrics.front().certified);

  const auto analysis = detect(cmp);
  MPI_CHECK_EQ(analysis.issues.size(), std::size_t{1});
  if (analysis.issues.empty()) return;
  MPI_CHECK(mentions(analysis.issues.front().missing_evidence,
                     "certifies nothing about release performance"));
  MPI_CHECK(mentions(analysis.data_quality_notes, "certified benchmark pair"));
}

MPI_TEST(det08_refuses_a_cross_platform_pair, {"DET-08", "I19"}) {
  const auto cmp = compare(make_set("baseline", {100e6, 101e6, 99e6, 100e6, 102e6}, "android"),
                           make_set("candidate", {130e6, 132e6, 129e6, 131e6, 130e6}, "ios"),
                           default_thresholds());
  const auto analysis = detect(cmp);
  MPI_CHECK(analysis.issues.empty());
  const auto* rec = det08_record(analysis);
  MPI_CHECK(rec != nullptr);
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK(mentions(rec->skipped_reasons, "two platforms"));
}

MPI_TEST(det08_skips_when_conditions_are_not_comparable, {"DET-08", "I02"}) {
  auto candidate = make_set("candidate", {130e6, 132e6, 129e6, 131e6, 130e6});
  candidate.conditions.device_model = "Pixel 6";
  const auto analysis = detect(compare(
      make_set("baseline", {100e6, 101e6, 99e6, 100e6, 102e6}), std::move(candidate),
      default_thresholds()));
  MPI_CHECK(analysis.issues.empty());
  const auto* rec = det08_record(analysis);
  MPI_CHECK(rec != nullptr);
  MPI_CHECK(mentions(rec->skipped_reasons, "not comparable"));
}

MPI_TEST(det08_records_an_undecided_metric_rather_than_passing_it,
         {"DET-08", "I09", "I10", "H11"}) {
  // Too few runs on each side: the comparison cannot decide, and the detector
  // must say so instead of reporting a clean result.
  const auto cmp = compare(make_set("baseline", {100e6, 101e6}),
                           make_set("candidate", {160e6, 162e6}),
                           default_thresholds());
  MPI_CHECK(cmp.overall == ComparisonVerdict::kInconclusive);

  const auto analysis = detect(cmp);
  MPI_CHECK(analysis.issues.empty());
  const auto* rec = det08_record(analysis);
  MPI_CHECK(rec != nullptr);
  // It ran: the metric was examined and came back undecided. That is a
  // different statement from the rule not running at all.
  MPI_CHECK(rec->outcome == model::RuleOutcome::kRanFoundNothing);
  MPI_CHECK(mentions(rec->skipped_reasons, "inconclusive"));
}

MPI_TEST(det08_does_not_report_an_improvement_as_an_issue, {"DET-08"}) {
  const auto analysis = detect(compare(
      make_set("baseline", {130e6, 132e6, 129e6, 131e6, 130e6}),
      make_set("candidate", {100e6, 101e6, 99e6, 100e6, 102e6}),
      default_thresholds()));
  MPI_CHECK(analysis.issues.empty());
  const auto* rec = det08_record(analysis);
  MPI_CHECK(rec != nullptr);
  MPI_CHECK(rec->outcome == model::RuleOutcome::kRanFoundNothing);
  MPI_CHECK(mentions(rec->skipped_reasons, "improvement"));
}

// --- reading a run set ------------------------------------------------------
// The loader lived in the CLI until DevX needed it, which meant these rules
// had no test at all: what an absent field means is part of the comparison's
// honesty, and it is exactly where a second implementation would have drifted.
namespace {

std::string write_temp(const std::string& name, const std::string& body) {
  const char* base = std::getenv("TMPDIR");
  std::string path = (base ? std::string(base) : std::string("/tmp/")) + name;
  std::ofstream f(path);
  f << body;
  return path;
}

}  // namespace

MPI_TEST(a_run_without_a_stated_outcome_is_excluded, {"I15"}) {
  // Absent `scenario_completed` means nobody said whether the scenario
  // finished, and a run that may not have finished cannot count toward a
  // median. Defaulting it to true would quietly admit broken runs.
  const auto path = write_temp("mpi-runset-outcome.json", R"({
    "conditions": {"platform": "android", "scenario_id": "s",
                   "scenario_version": "1", "device_form": "physical"},
    "runs": [
      {"session_id": "a", "metric_name": "m", "unit": "ns", "value": 100,
       "scenario_completed": true},
      {"session_id": "b", "metric_name": "m", "unit": "ns", "value": 900}
    ]
  })");
  RunSet set;
  std::string error;
  MPI_CHECK(read_run_set(path, "baseline", set, error));
  MPI_CHECK_EQ(set.runs.size(), std::size_t{2});
  if (set.runs.size() < 2) return;
  MPI_CHECK(set.runs[0].scenario_completed);
  MPI_CHECK_MSG(!set.runs[1].scenario_completed,
                "an unstated outcome is not a completed scenario");
  MPI_CHECK(!set.runs[1].exclusion_reason.empty());
  MPI_CHECK(set.runs[1].exclusion_reason.find("not stated") !=
            std::string::npos);
  // And the value really is kept out of the median rather than merely flagged.
  const auto valid = set.valid_values("m");
  MPI_CHECK_EQ(valid.size(), std::size_t{1});
  if (!valid.empty()) MPI_CHECK_EQ(valid.front(), 100.0);
}

MPI_TEST(an_unstated_eligibility_is_insufficient_evidence, {"I02", "H01"}) {
  // Not a pass. A run set that says nothing about whether it is a certifiable
  // benchmark has not certified itself.
  const auto path = write_temp("mpi-runset-elig.json", R"({
    "conditions": {"platform": "android", "scenario_id": "s",
                   "scenario_version": "1", "device_form": "physical"},
    "runs": [{"session_id": "a", "metric_name": "m", "unit": "ns",
              "value": 1, "scenario_completed": true}]
  })");
  RunSet set;
  std::string error;
  MPI_CHECK(read_run_set(path, "baseline", set, error));
  MPI_CHECK(set.eligibility.status ==
            model::EligibilityStatus::kInsufficientEvidence);
  MPI_CHECK(!set.eligibility.reasons.empty());
  MPI_CHECK(!set.eligibility.user_override);
}

MPI_TEST(a_run_set_without_conditions_is_refused, {"I03"}) {
  // Comparability is decided from the conditions. Without them the loader
  // cannot know whether two sides are comparable, so it refuses rather than
  // treating "unstated" as "matching".
  const auto path = write_temp("mpi-runset-nocond.json",
                               R"({"runs": []})");
  RunSet set;
  std::string error;
  MPI_CHECK(!read_run_set(path, "baseline", set, error));
  MPI_CHECK(error.find("conditions") != std::string::npos);
  MPI_CHECK(error.find("comparability") != std::string::npos);
}

MPI_TEST(a_run_set_without_runs_is_refused, {"I15"}) {
  const auto path = write_temp("mpi-runset-noruns.json", R"({
    "conditions": {"platform": "android", "scenario_id": "s",
                   "scenario_version": "1", "device_form": "physical"}
  })");
  RunSet set;
  std::string error;
  MPI_CHECK(!read_run_set(path, "baseline", set, error));
  MPI_CHECK(error.find("runs") != std::string::npos);
}

MPI_TEST(a_run_with_no_value_keeps_its_place, {"I15", "H01"}) {
  // A run that produced no measurement is not a run that measured zero. It
  // stays in the set -- so the count of attempted runs is honest -- and out
  // of the values.
  const auto path = write_temp("mpi-runset-novalue.json", R"({
    "conditions": {"platform": "android", "scenario_id": "s",
                   "scenario_version": "1", "device_form": "physical"},
    "runs": [
      {"session_id": "a", "metric_name": "m", "unit": "ns", "value": 10,
       "scenario_completed": true},
      {"session_id": "b", "metric_name": "m", "unit": "ns",
       "scenario_completed": true}
    ]
  })");
  RunSet set;
  std::string error;
  MPI_CHECK(read_run_set(path, "baseline", set, error));
  MPI_CHECK_EQ(set.runs.size(), std::size_t{2});
  if (set.runs.size() < 2) return;
  MPI_CHECK(set.runs[0].value.has_value());
  MPI_CHECK_MSG(!set.runs[1].value.has_value(),
                "a run with no measurement carries no value, not a zero");
  MPI_CHECK_EQ(set.valid_values("m").size(), std::size_t{1});
}

MPI_TEST(a_missing_file_is_an_error_not_an_empty_set, {"A12"}) {
  RunSet set;
  std::string error;
  MPI_CHECK(!read_run_set("/nonexistent/mpi-run-set.json", "baseline", set,
                          error));
  MPI_CHECK(!error.empty());
  MPI_CHECK_MSG(set.runs.empty(),
                "a failed read leaves nothing behind that could be compared");
}

MPI_TEST(a_warm_up_run_is_marked_as_one, {"I16"}) {
  const auto path = write_temp("mpi-runset-warm.json", R"({
    "conditions": {"platform": "android", "scenario_id": "s",
                   "scenario_version": "1", "device_form": "physical"},
    "runs": [{"session_id": "a", "metric_name": "m", "unit": "ns",
              "value": 1, "scenario_completed": true, "warm_up": true}]
  })");
  RunSet set;
  std::string error;
  MPI_CHECK(read_run_set(path, "baseline", set, error));
  MPI_CHECK_EQ(set.runs.size(), std::size_t{1});
  if (set.runs.empty()) return;
  MPI_CHECK(set.runs.front().warm_up);
}
