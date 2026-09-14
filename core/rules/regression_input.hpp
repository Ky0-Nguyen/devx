// What DET-08 needs to know about a baseline/candidate comparison.
//
// The significance decision belongs to the comparison engine
// (`core/session/compare`), which already knows about run counts, spread,
// exclusions and condition mismatches. This carries that result into the rule
// so the detector renders it as an issue rather than deciding again: two
// places deciding what counts as a regression would eventually disagree, and
// the one in the report would be the one nobody tested.
//
// It is a rules-level type on purpose. `core/session` already depends on
// `core/rules`, so the translation lives on the session side
// (`session::to_regression_input`) and the dependency stays one-way.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mpi::rules {

struct RegressionMetric {
  std::string metric_name;
  std::string unit;
  // "regression" | "improvement" | "no_significant_change" | "inconclusive",
  // as decided by the comparison engine.
  std::string verdict;
  std::vector<std::string> reasons;

  std::optional<double> baseline_median;
  std::optional<double> candidate_median;
  std::optional<double> absolute_delta;
  std::optional<double> relative_delta;
  std::optional<double> baseline_spread;
  std::optional<double> candidate_spread;
  std::size_t baseline_valid_runs = 0;
  std::size_t candidate_valid_runs = 0;
  // Runs dropped before the medians were taken, with their reasons, so an
  // exclusion can never be invisible (spec I14).
  std::vector<std::string> excluded_runs;
  // True only when both sides are certified benchmark-eligible. A comparison
  // of two diagnostic runs measures a real difference in that configuration
  // and nothing about release performance (spec I16).
  bool certified = false;
};

struct RegressionInput {
  std::string scenario_id;
  std::string scenario_version;
  std::string baseline_label;
  std::string candidate_label;
  // Condition mismatches that invalidate the whole comparison (I01-I08, I17).
  std::vector<std::string> incompatibilities;
  // An android/ios pair may be shown side by side but never gates (I19).
  bool cross_platform = false;

  std::size_t min_valid_runs = 5;
  double min_relative_delta = 0.05;
  double min_absolute_delta = 0.0;
  double max_relative_spread = 0.25;

  std::vector<RegressionMetric> metrics;
};

}  // namespace mpi::rules
