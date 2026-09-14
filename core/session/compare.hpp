// Benchmark comparison (spec section 12).
//
// The default answer is "inconclusive". A verdict requires matching
// conditions, enough valid runs, and an effect that clears both an absolute
// and a relative threshold. A crash or a missing endpoint is never a fast
// successful run, and a debug-vs-release pair never produces a certified
// comparison.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model/trace.hpp"
#include "core/rules/regression_input.hpp"
#include "core/util/json.hpp"

namespace mpi::session {

// One measured run of a versioned scenario.
struct RunMeasurement {
  std::string session_id;
  std::string metric_name;
  std::string unit;
  std::optional<double> value;  // absent = the run produced no measurement
  // A run that did not complete the scenario is excluded, with the reason
  // visible (spec I15).
  bool scenario_completed = true;
  std::string exclusion_reason;
  bool warm_up = false;
  json::Value to_json() const;
};

// Everything that must match before two sides are comparable.
struct RunConditions {
  // "android" | "ios". A pair that differs here can be displayed side by side
  // but may never drive a regression gate (spec I19).
  std::string platform;
  std::string scenario_id;
  std::string scenario_version;
  std::string device_id;
  std::string device_model;
  std::string device_form;  // physical / simulator / emulator
  std::string os_version;
  std::string refresh_policy;
  std::string collector_preset;
  std::optional<double> collector_sample_rate_hz;
  std::string launch_class;  // cold / warm / hot
  std::string thermal_state;
  std::string power_state;
  std::string input_data_version;
  std::string account_state;
  std::string network_condition;
  std::string cache_state;

  json::Value to_json() const;
};

struct RunSet {
  std::string label;  // "baseline" / "candidate"
  RunConditions conditions;
  model::MeasurementMode mode = model::MeasurementMode::kUnknownLimited;
  model::Eligibility eligibility;
  std::vector<RunMeasurement> runs;

  std::vector<double> valid_values(const std::string& metric_name) const;
  json::Value to_json() const;
};

enum class ComparisonVerdict {
  kRegression,
  kImprovement,
  kNoSignificantChange,
  kInconclusive,
};
const char* to_string(ComparisonVerdict v);

struct MetricComparison {
  std::string metric_name;
  std::string unit;
  ComparisonVerdict verdict = ComparisonVerdict::kInconclusive;
  std::vector<std::string> reasons;

  std::optional<double> baseline_median;
  std::optional<double> candidate_median;
  std::optional<double> absolute_delta;
  std::optional<double> relative_delta;
  // Interquartile-style spread, reported instead of a single number.
  std::optional<double> baseline_spread;
  std::optional<double> candidate_spread;
  std::size_t baseline_valid_runs = 0;
  std::size_t candidate_valid_runs = 0;
  std::vector<std::string> excluded_runs;
  // True only when both sides are certified-eligible benchmarks.
  bool certified = false;

  json::Value to_json() const;
};

struct ComparisonThresholds {
  // Both must be cleared for a verdict other than no-significant-change.
  double min_absolute_delta = 0.0;
  double min_relative_delta = 0.05;
  std::size_t min_valid_runs = 5;
  // Above this coefficient of variation the result is inconclusive (spec I10).
  double max_relative_spread = 0.25;
};

struct ComparisonResult {
  RunSet baseline;
  RunSet candidate;
  ComparisonThresholds thresholds;
  std::vector<MetricComparison> metrics;
  // Condition mismatches that make the whole comparison invalid.
  std::vector<std::string> incompatibilities;
  // Cross-platform comparison is refused as a regression gate (spec I19).
  bool cross_platform = false;
  ComparisonVerdict overall = ComparisonVerdict::kInconclusive;

  json::Value to_json() const;
};

// Compares two run sets. Never returns a regression verdict when conditions
// are incompatible, runs are too few, or variance is too high.
ComparisonResult compare(RunSet baseline, RunSet candidate,
                         const ComparisonThresholds& thresholds);

// Hands the comparison's own conclusions to DET-08, which renders them as
// issues. The translation lives here because this layer already depends on
// the rules layer; the detector never recomputes significance.
rules::RegressionInput to_regression_input(const ComparisonResult& result);

}  // namespace mpi::session
