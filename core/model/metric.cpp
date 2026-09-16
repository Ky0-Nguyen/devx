#include "core/model/metric.hpp"

namespace mpi::model {

const char* to_string(MetricMethod m) {
  switch (m) {
    case MetricMethod::kMeasured: return "measured";
    case MetricMethod::kSampled: return "sampled";
    case MetricMethod::kDerived: return "derived";
  }
  return "derived";
}

std::optional<double> cpu_utilisation_percent(TimeNs earlier_at_ns,
                                              double earlier_cpu_ns,
                                              TimeNs at_ns, double cpu_ns) {
  if (at_ns <= earlier_at_ns) return std::nullopt;
  const double cpu = cpu_ns - earlier_cpu_ns;
  if (cpu < 0.0) return std::nullopt;
  const double wall = static_cast<double>(at_ns - earlier_at_ns);
  return cpu / wall * 100.0;
}

const char* to_string(CpuNormalization n) {
  switch (n) {
    case CpuNormalization::kSingleCore: return "single_core";
    case CpuNormalization::kAllCores: return "all_cores";
    case CpuNormalization::kNotApplicable: return "not_applicable";
    case CpuNormalization::kUnknown: return "unknown";
  }
  return "unknown";
}

json::Value Metric::to_json() const {
  json::Value v = json::Value::object();
  v.set("name", json::Value::string(name));
  v.set("unit", json::Value::string(unit));
  // Absent value stays null. A metric that was not collected is not 0.
  v.set("value", value.has_value() ? json::Value::number(*value) : json::Value::null());
  v.set("measured", json::Value::boolean(measured()));
  v.set("provider", json::Value::string(provider));
  v.set("process_instance_id", process_instance_id.empty()
                                   ? json::Value::null()
                                   : json::Value::string(process_instance_id));
  v.set("thread_instance_id", thread_instance_id.empty()
                                  ? json::Value::null()
                                  : json::Value::string(thread_instance_id));
  v.set("app_scoped", json::Value::boolean(app_scoped));
  v.set("window_start_ns", json::Value::integer(window_start_ns));
  v.set("window_end_ns", json::Value::integer(window_end_ns));
  v.set("method", json::Value::string(to_string(method)));
  v.set("aggregation", json::Value::string(aggregation));
  v.set("cpu_normalization", json::Value::string(to_string(cpu_normalization)));
  v.set("coverage_fraction", coverage_fraction.has_value()
                                 ? json::Value::number(*coverage_fraction)
                                 : json::Value::null());
  json::Value lim = json::Value::array();
  for (const auto& s : limitations) lim.push_back(json::Value::string(s));
  v.set("limitations", std::move(lim));
  json::Value fl = json::Value::array();
  for (const auto f : quality_flags) fl.push_back(json::Value::string(to_string(f)));
  v.set("quality_flags", std::move(fl));
  return v;
}

const char* to_string(AttributionCategory c) {
  switch (c) {
    case AttributionCategory::kApplication: return "application";
    case AttributionCategory::kDevelopmentTooling: return "development_tooling";
    case AttributionCategory::kProfiler: return "profiler";
    case AttributionCategory::kSharedOrUnknown: return "shared_or_unknown";
  }
  return "shared_or_unknown";
}

const char* to_string(AttributionBasis b) {
  switch (b) {
    case AttributionBasis::kKnownOwnership: return "known_ownership";
    case AttributionBasis::kInstrumentation: return "instrumentation";
    case AttributionBasis::kMatchingStack: return "matching_stack";
    case AttributionBasis::kVerifiedEndpoint: return "verified_endpoint";
    case AttributionBasis::kUnclassified: return "unclassified";
  }
  return "unclassified";
}

json::Value AttributedSlice::to_json() const {
  json::Value v = json::Value::object();
  v.set("category", json::Value::string(to_string(category)));
  v.set("basis", json::Value::string(to_string(basis)));
  v.set("label", json::Value::string(label));
  v.set("rule_id", json::Value::string(rule_id));
  v.set("rule_version", json::Value::string(rule_version));
  v.set("value", value.has_value() ? json::Value::number(*value) : json::Value::null());
  v.set("unit", json::Value::string(unit));
  v.set("locus", json::Value::string(locus));
  json::Value e = json::Value::array();
  for (const auto& s : evidence_refs) e.push_back(json::Value::string(s));
  v.set("evidence_refs", std::move(e));
  return v;
}

json::Value AttributionReport::to_json() const {
  json::Value v = json::Value::object();
  v.set("original_total", original_total.to_json());
  json::Value s = json::Value::array();
  for (const auto& x : slices) s.push_back(x.to_json());
  v.set("slices", std::move(s));
  v.set("slices_are_disjoint", json::Value::boolean(slices_are_disjoint));
  // Spec section 9 rule 6 is a property of the model, not a UI disclaimer.
  v.set("subtraction_to_estimate_release_permitted",
        json::Value::boolean(false));
  json::Value lim = json::Value::array();
  for (const auto& x : limitations) lim.push_back(json::Value::string(x));
  v.set("limitations", std::move(lim));
  return v;
}

}  // namespace mpi::model
