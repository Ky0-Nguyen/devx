#include "core/session/compare.hpp"

#include <algorithm>
#include <cmath>

namespace mpi::session {
namespace {

std::optional<double> median(std::vector<double> v) {
  if (v.empty()) return std::nullopt;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n % 2 == 1) return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Interquartile range. Reported as the spread rather than a standard
// deviation, which a handful of runs cannot support.
std::optional<double> iqr(std::vector<double> v) {
  if (v.size() < 4) return std::nullopt;
  std::sort(v.begin(), v.end());
  const auto q = [&](double p) {
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
  };
  return q(0.75) - q(0.25);
}

// Each entry: field accessor, field label. A mismatch in any of these makes
// the two sides incomparable.
void check_conditions(const RunConditions& a, const RunConditions& b,
                      std::vector<std::string>& out) {
  struct Field {
    const char* label;
    const std::string* a;
    const std::string* b;
  };
  const Field fields[] = {
      {"scenario_id", &a.scenario_id, &b.scenario_id},
      {"scenario_version", &a.scenario_version, &b.scenario_version},
      {"device_model", &a.device_model, &b.device_model},
      {"device_form", &a.device_form, &b.device_form},
      {"os_version", &a.os_version, &b.os_version},
      {"refresh_policy", &a.refresh_policy, &b.refresh_policy},
      {"collector_preset", &a.collector_preset, &b.collector_preset},
      {"launch_class", &a.launch_class, &b.launch_class},
      {"input_data_version", &a.input_data_version, &b.input_data_version},
      {"account_state", &a.account_state, &b.account_state},
      {"network_condition", &a.network_condition, &b.network_condition},
      {"cache_state", &a.cache_state, &b.cache_state},
      {"thermal_state", &a.thermal_state, &b.thermal_state},
      {"power_state", &a.power_state, &b.power_state},
  };
  for (const auto& f : fields) {
    if (f.a->empty() && f.b->empty()) {
      out.push_back(std::string(f.label) +
                    " is unknown on both sides; comparability cannot be "
                    "established");
      continue;
    }
    if (*f.a != *f.b) {
      out.push_back(std::string(f.label) + " differs: baseline '" + *f.a +
                    "' vs candidate '" + *f.b + "'");
    }
  }
  if (a.collector_sample_rate_hz.has_value() !=
          b.collector_sample_rate_hz.has_value() ||
      (a.collector_sample_rate_hz.has_value() &&
       std::abs(*a.collector_sample_rate_hz - *b.collector_sample_rate_hz) > 1e-9)) {
    // Spec I17: a sample-rate mismatch changes what the numbers mean.
    out.push_back("collector sample rate differs between the two sides");
  }
}

}  // namespace

json::Value RunMeasurement::to_json() const {
  json::Value v = json::Value::object();
  v.set("session_id", json::Value::string(session_id));
  v.set("metric_name", json::Value::string(metric_name));
  v.set("unit", json::Value::string(unit));
  v.set("value", value.has_value() ? json::Value::number(*value) : json::Value::null());
  v.set("scenario_completed", json::Value::boolean(scenario_completed));
  v.set("exclusion_reason", exclusion_reason.empty()
                                ? json::Value::null()
                                : json::Value::string(exclusion_reason));
  v.set("warm_up", json::Value::boolean(warm_up));
  return v;
}

json::Value RunConditions::to_json() const {
  json::Value v = json::Value::object();
  auto s = [&](const char* k, const std::string& x) {
    v.set(k, x.empty() ? json::Value::null() : json::Value::string(x));
  };
  s("platform", platform);
  s("scenario_id", scenario_id);
  s("scenario_version", scenario_version);
  s("device_id", device_id);
  s("device_model", device_model);
  s("device_form", device_form);
  s("os_version", os_version);
  s("refresh_policy", refresh_policy);
  s("collector_preset", collector_preset);
  v.set("collector_sample_rate_hz",
        collector_sample_rate_hz.has_value()
            ? json::Value::number(*collector_sample_rate_hz)
            : json::Value::null());
  s("launch_class", launch_class);
  s("thermal_state", thermal_state);
  s("power_state", power_state);
  s("input_data_version", input_data_version);
  s("account_state", account_state);
  s("network_condition", network_condition);
  s("cache_state", cache_state);
  return v;
}

std::vector<double> RunSet::valid_values(const std::string& metric_name) const {
  std::vector<double> out;
  for (const auto& r : runs) {
    if (r.metric_name != metric_name) continue;
    if (r.warm_up) continue;            // warm-ups are never measured runs
    if (!r.scenario_completed) continue;  // spec I15
    if (!r.exclusion_reason.empty()) continue;
    if (!r.value.has_value()) continue;
    out.push_back(*r.value);
  }
  return out;
}

json::Value RunSet::to_json() const {
  json::Value v = json::Value::object();
  v.set("label", json::Value::string(label));
  v.set("conditions", conditions.to_json());
  v.set("measurement_mode", json::Value::string(model::to_string(mode)));
  v.set("benchmark_eligibility", eligibility.to_json());
  json::Value rs = json::Value::array();
  for (const auto& r : runs) rs.push_back(r.to_json());
  v.set("runs", std::move(rs));
  return v;
}

const char* to_string(ComparisonVerdict v) {
  switch (v) {
    case ComparisonVerdict::kRegression: return "regression";
    case ComparisonVerdict::kImprovement: return "improvement";
    case ComparisonVerdict::kNoSignificantChange: return "no_significant_change";
    case ComparisonVerdict::kInconclusive: return "inconclusive";
  }
  return "inconclusive";
}

json::Value MetricComparison::to_json() const {
  json::Value v = json::Value::object();
  v.set("metric_name", json::Value::string(metric_name));
  v.set("unit", json::Value::string(unit));
  v.set("verdict", json::Value::string(to_string(verdict)));
  json::Value rs = json::Value::array();
  for (const auto& r : reasons) rs.push_back(json::Value::string(r));
  v.set("reasons", std::move(rs));
  auto num = [](const std::optional<double>& d) {
    return d.has_value() ? json::Value::number(*d) : json::Value::null();
  };
  v.set("baseline_median", num(baseline_median));
  v.set("candidate_median", num(candidate_median));
  v.set("absolute_delta", num(absolute_delta));
  v.set("relative_delta", num(relative_delta));
  v.set("baseline_spread_iqr", num(baseline_spread));
  v.set("candidate_spread_iqr", num(candidate_spread));
  v.set("baseline_valid_runs",
        json::Value::integer(static_cast<std::int64_t>(baseline_valid_runs)));
  v.set("candidate_valid_runs",
        json::Value::integer(static_cast<std::int64_t>(candidate_valid_runs)));
  json::Value ex = json::Value::array();
  for (const auto& e : excluded_runs) ex.push_back(json::Value::string(e));
  v.set("excluded_runs", std::move(ex));
  v.set("certified", json::Value::boolean(certified));
  return v;
}

json::Value ComparisonResult::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  v.set("baseline", baseline.to_json());
  v.set("candidate", candidate.to_json());
  json::Value th = json::Value::object();
  th.set("min_absolute_delta", json::Value::number(thresholds.min_absolute_delta));
  th.set("min_relative_delta", json::Value::number(thresholds.min_relative_delta));
  th.set("min_valid_runs",
         json::Value::integer(static_cast<std::int64_t>(thresholds.min_valid_runs)));
  th.set("max_relative_spread", json::Value::number(thresholds.max_relative_spread));
  v.set("thresholds", std::move(th));
  json::Value ms = json::Value::array();
  for (const auto& m : metrics) ms.push_back(m.to_json());
  v.set("metrics", std::move(ms));
  json::Value inc = json::Value::array();
  for (const auto& i : incompatibilities) inc.push_back(json::Value::string(i));
  v.set("incompatibilities", std::move(inc));
  v.set("cross_platform", json::Value::boolean(cross_platform));
  // Spec I19: a cross-platform pair may be displayed but never gated.
  v.set("usable_as_regression_gate",
        json::Value::boolean(!cross_platform && incompatibilities.empty()));
  v.set("overall_verdict", json::Value::string(to_string(overall)));
  return v;
}

ComparisonResult compare(RunSet baseline_in, RunSet candidate_in,
                         const ComparisonThresholds& thresholds) {
  ComparisonResult res;
  res.baseline = std::move(baseline_in);
  res.candidate = std::move(candidate_in);
  res.thresholds = thresholds;

  check_conditions(res.baseline.conditions, res.candidate.conditions,
                   res.incompatibilities);

  // A simulator/emulator side is never comparable to a physical device.
  const std::string& bf = res.baseline.conditions.device_form;
  const std::string& cf = res.candidate.conditions.device_form;
  if (bf != cf && !bf.empty() && !cf.empty()) {
    res.incompatibilities.push_back(
        "device form differs (" + bf + " vs " + cf +
        "); simulator and physical measurements are not interchangeable");
  }

  // Debug vs release is not a certified comparison (spec I16).
  if (res.baseline.mode != res.candidate.mode) {
    res.incompatibilities.push_back(
        std::string("measurement mode differs (") +
        model::to_string(res.baseline.mode) + " vs " +
        model::to_string(res.candidate.mode) +
        "); this cannot certify a performance change");
  }

  const bool certified = res.baseline.eligibility.certified_benchmark() &&
                         res.candidate.eligibility.certified_benchmark();
  if (!certified) {
    for (const auto& r : res.baseline.eligibility.reasons) {
      res.incompatibilities.push_back("baseline not benchmark-eligible: " + r);
    }
    for (const auto& r : res.candidate.eligibility.reasons) {
      res.incompatibilities.push_back("candidate not benchmark-eligible: " + r);
    }
  }

  // A cross-platform pair has no equivalent metric semantics, so it is
  // refused as a regression gate outright (spec I19). It is not merely a
  // warning: it is added to the incompatibilities that force inconclusive.
  const std::string& bp = res.baseline.conditions.platform;
  const std::string& cp = res.candidate.conditions.platform;
  if (!bp.empty() && !cp.empty() && bp != cp) {
    res.cross_platform = true;
    res.incompatibilities.push_back(
        "cross-platform comparison (" + bp + " vs " + cp +
        "): the two sides do not share metric semantics, so this pair cannot "
        "be used as a regression gate");
  } else if (bp.empty() || cp.empty()) {
    res.incompatibilities.push_back(
        "platform is unknown on at least one side; equivalence cannot be "
        "established");
  }

  // Collect the union of metric names present on either side.
  std::vector<std::string> names;
  for (const auto* set : {&res.baseline, &res.candidate}) {
    for (const auto& r : set->runs) {
      if (std::find(names.begin(), names.end(), r.metric_name) == names.end()) {
        names.push_back(r.metric_name);
      }
    }
  }
  std::sort(names.begin(), names.end());

  bool any_regression = false;
  bool any_improvement = false;
  bool any_conclusive = false;

  for (const auto& name : names) {
    MetricComparison mc;
    mc.metric_name = name;
    mc.certified = certified;
    for (const auto& r : res.baseline.runs) {
      if (r.metric_name == name && !r.unit.empty()) {
        mc.unit = r.unit;
        break;
      }
    }

    auto bv = res.baseline.valid_values(name);
    auto cv = res.candidate.valid_values(name);
    mc.baseline_valid_runs = bv.size();
    mc.candidate_valid_runs = cv.size();

    for (const auto* set : {&res.baseline, &res.candidate}) {
      for (const auto& r : set->runs) {
        if (r.metric_name != name) continue;
        if (r.warm_up) {
          mc.excluded_runs.push_back(set->label + "/" + r.session_id + ": warm-up run");
        } else if (!r.scenario_completed) {
          mc.excluded_runs.push_back(set->label + "/" + r.session_id +
                                     ": scenario did not complete (not a fast run)");
        } else if (!r.exclusion_reason.empty()) {
          mc.excluded_runs.push_back(set->label + "/" + r.session_id + ": " +
                                     r.exclusion_reason);
        } else if (!r.value.has_value()) {
          mc.excluded_runs.push_back(set->label + "/" + r.session_id +
                                     ": no measurement produced");
        }
      }
    }

    mc.baseline_median = median(bv);
    mc.candidate_median = median(cv);
    mc.baseline_spread = iqr(bv);
    mc.candidate_spread = iqr(cv);

    if (!res.incompatibilities.empty()) {
      mc.verdict = ComparisonVerdict::kInconclusive;
      mc.reasons.push_back(
          "run conditions or eligibility are incompatible; see the "
          "incompatibilities list");
      res.metrics.push_back(std::move(mc));
      continue;
    }
    if (bv.size() < thresholds.min_valid_runs ||
        cv.size() < thresholds.min_valid_runs) {
      mc.verdict = ComparisonVerdict::kInconclusive;
      mc.reasons.push_back(
          "too few valid runs: " + std::to_string(bv.size()) + " baseline and " +
          std::to_string(cv.size()) + " candidate, minimum " +
          std::to_string(thresholds.min_valid_runs) + " each");
      res.metrics.push_back(std::move(mc));
      continue;
    }
    if (!mc.baseline_median.has_value() || !mc.candidate_median.has_value()) {
      mc.verdict = ComparisonVerdict::kInconclusive;
      mc.reasons.push_back("no median could be computed on one or both sides");
      res.metrics.push_back(std::move(mc));
      continue;
    }

    // High variance on either side makes the comparison inconclusive.
    const auto relative_spread = [&](const std::optional<double>& spread,
                                     double med) -> std::optional<double> {
      if (!spread.has_value() || med == 0.0) return std::nullopt;
      return *spread / std::abs(med);
    };
    const auto brs = relative_spread(mc.baseline_spread, *mc.baseline_median);
    const auto crs = relative_spread(mc.candidate_spread, *mc.candidate_median);
    if ((brs.has_value() && *brs > thresholds.max_relative_spread) ||
        (crs.has_value() && *crs > thresholds.max_relative_spread)) {
      mc.verdict = ComparisonVerdict::kInconclusive;
      mc.reasons.push_back(
          "run-to-run spread exceeds the maximum relative spread of " +
          std::to_string(thresholds.max_relative_spread) +
          "; the difference cannot be distinguished from noise");
      res.metrics.push_back(std::move(mc));
      continue;
    }

    mc.absolute_delta = *mc.candidate_median - *mc.baseline_median;
    // Spec I12: a zero baseline must not produce a division.
    if (*mc.baseline_median == 0.0) {
      mc.relative_delta = std::nullopt;
      mc.reasons.push_back(
          "baseline median is zero, so no relative change is defined; only the "
          "absolute delta is reported");
    } else {
      mc.relative_delta = *mc.absolute_delta / std::abs(*mc.baseline_median);
    }

    const bool clears_absolute =
        std::abs(*mc.absolute_delta) >= thresholds.min_absolute_delta;
    const bool clears_relative =
        mc.relative_delta.has_value() &&
        std::abs(*mc.relative_delta) >= thresholds.min_relative_delta;

    // Spec section 12: both an absolute and a relative threshold must clear.
    if (clears_absolute && clears_relative) {
      any_conclusive = true;
      if (*mc.absolute_delta > 0) {
        mc.verdict = ComparisonVerdict::kRegression;
        any_regression = true;
        mc.reasons.push_back("candidate median is higher than baseline by " +
                             std::to_string(*mc.absolute_delta) + " " + mc.unit);
      } else {
        mc.verdict = ComparisonVerdict::kImprovement;
        any_improvement = true;
        mc.reasons.push_back("candidate median is lower than baseline by " +
                             std::to_string(-*mc.absolute_delta) + " " + mc.unit);
      }
      mc.reasons.push_back(
          "direction assumes a lower value is better for this metric");
    } else {
      mc.verdict = ComparisonVerdict::kNoSignificantChange;
      any_conclusive = true;
      if (!clears_absolute) {
        mc.reasons.push_back("absolute delta below the minimum of " +
                             std::to_string(thresholds.min_absolute_delta));
      }
      if (!clears_relative) {
        mc.reasons.push_back("relative delta below the minimum of " +
                             std::to_string(thresholds.min_relative_delta));
      }
    }
    res.metrics.push_back(std::move(mc));
  }

  if (!res.incompatibilities.empty() || !any_conclusive) {
    res.overall = ComparisonVerdict::kInconclusive;
  } else if (any_regression) {
    res.overall = ComparisonVerdict::kRegression;
  } else if (any_improvement) {
    res.overall = ComparisonVerdict::kImprovement;
  } else {
    res.overall = ComparisonVerdict::kNoSignificantChange;
  }
  return res;
}

}  // namespace mpi::session
