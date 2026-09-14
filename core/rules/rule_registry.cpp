#include "core/rules/rule_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace mpi::rules {

// Declared in the individual detector translation units.
RulePtr make_det01_frame_deadlines();
RulePtr make_det02_long_js();
RulePtr make_det04_cpu_hotspot();
RulePtr make_det03_main_thread_io();
RulePtr make_det05_memory_growth();
RulePtr make_det07_startup_budget();
RulePtr make_det08_regression();
RulePtr make_det09_wait_contention();
RulePtr make_det10_react_renders();
RulePtr make_det11_network_delay();
RulePtr make_det12_tooling_activity();
std::vector<RulePtr> make_deferred_rules();

std::string ruleset_version() {
  // major.minor: minor changes when a threshold default moves, major when a
  // rule's conclusion semantics change.
  return "1.0";
}

std::vector<RulePtr> all_rules() {
  std::vector<RulePtr> rules;
  rules.push_back(make_det01_frame_deadlines());
  rules.push_back(make_det02_long_js());
  rules.push_back(make_det04_cpu_hotspot());
  rules.push_back(make_det03_main_thread_io());
  rules.push_back(make_det05_memory_growth());
  rules.push_back(make_det07_startup_budget());
  rules.push_back(make_det08_regression());
  rules.push_back(make_det09_wait_contention());
  rules.push_back(make_det10_react_renders());
  rules.push_back(make_det11_network_delay());
  rules.push_back(make_det12_tooling_activity());
  for (auto& r : make_deferred_rules()) rules.push_back(std::move(r));
  std::sort(rules.begin(), rules.end(),
            [](const RulePtr& a, const RulePtr& b) { return a->id() < b->id(); });
  return rules;
}

const char* to_string(ThresholdOrigin o) {
  switch (o) {
    case ThresholdOrigin::kProjectBudget: return "project_budget";
    case ThresholdOrigin::kPlatformDeadlineObserved:
      return "platform_deadline_observed";
    case ThresholdOrigin::kEmpiricalBaseline: return "empirical_baseline";
    case ThresholdOrigin::kConfigurableHeuristic: return "configurable_heuristic";
  }
  return "configurable_heuristic";
}

json::Value ThresholdSpec::to_json() const {
  json::Value v = json::Value::object();
  v.set("name", json::Value::string(name));
  v.set("value", json::Value::number(value));
  v.set("unit", json::Value::string(unit));
  v.set("origin", json::Value::string(to_string(origin)));
  v.set("rationale", json::Value::string(rationale));
  // Spec section 10.3: a heuristic must not be presented as a platform standard.
  v.set("is_platform_standard",
        json::Value::boolean(origin == ThresholdOrigin::kPlatformDeadlineObserved));
  return v;
}

std::optional<double> RuleContext::override_for(const std::string& key) const {
  for (const auto& kv : threshold_overrides) {
    if (kv.first == key) return kv.second;
  }
  return std::nullopt;
}

std::vector<model::MeasurementMode> Rule::supported_modes() const {
  return {model::MeasurementMode::kDiagnostic, model::MeasurementMode::kBenchmark,
          model::MeasurementMode::kUnknownLimited};
}

MappedInterval map_producer_interval(const model::NormalizedTrace& trace,
                                     const std::string& producer_domain,
                                     model::TimeNs start_ns,
                                     model::TimeNs end_ns) {
  MappedInterval out;
  out.start_ns = start_ns;
  out.end_ns = end_ns;
  out.domain = producer_domain;
  if (producer_domain.empty() ||
      producer_domain == trace.primary_clock_domain) {
    out.mapped = true;
    out.domain = trace.primary_clock_domain;
    return out;
  }
  const auto mapped_start = trace.map_to_primary(producer_domain, start_ns);
  const auto mapped_end = trace.map_to_primary(producer_domain, end_ns);
  if (!mapped_start.has_value() || !mapped_end.has_value()) return out;
  out.start_ns = *mapped_start;
  out.end_ns = *mapped_end;
  out.mapped = true;
  out.domain = trace.primary_clock_domain;
  return out;
}

RuleOutput Rule::run(const RuleContext& ctx) const {
  RuleOutput out;
  out.record.rule_id = id();
  out.record.rule_version = version();
  for (const auto& p : prerequisites()) {
    out.record.prerequisites.push_back(p.id + ": " + p.description);
  }

  const auto modes = supported_modes();
  if (std::find(modes.begin(), modes.end(), ctx.mode) == modes.end()) {
    out.record.outcome = model::RuleOutcome::kSkipped;
    out.record.skipped_reasons.push_back(
        std::string("rule does not support measurement mode '") +
        model::to_string(ctx.mode) + "'");
    return out;
  }

  auto unmet = unmet_prerequisites(ctx);
  if (!unmet.empty()) {
    out.record.outcome = model::RuleOutcome::kSkipped;
    out.record.skipped_reasons = std::move(unmet);
    return out;
  }

  if (ctx.cancel.cancelled()) {
    out.record.outcome = model::RuleOutcome::kSkipped;
    out.record.skipped_reasons.push_back("cancelled before evaluation");
    return out;
  }

  evaluate(ctx, out);
  out.record.issues_emitted = static_cast<std::int64_t>(out.issues.size());
  // The distinction spec H11 demands: this rule ran and found nothing, which
  // is a different statement from "this rule did not run".
  out.record.outcome = out.issues.empty() ? model::RuleOutcome::kRanFoundNothing
                                          : model::RuleOutcome::kRanFoundIssues;
  return out;
}

json::Value Rule::describe() const {
  json::Value v = json::Value::object();
  v.set("rule_id", json::Value::string(id()));
  v.set("rule_version", json::Value::string(version()));
  v.set("category", json::Value::string(category()));
  v.set("title", json::Value::string(title()));
  v.set("delivery_phase", json::Value::string(delivery_phase()));
  json::Value pres = json::Value::array();
  for (const auto& p : prerequisites()) {
    json::Value o = json::Value::object();
    o.set("id", json::Value::string(p.id));
    o.set("description", json::Value::string(p.description));
    pres.push_back(std::move(o));
  }
  v.set("prerequisites", std::move(pres));
  json::Value modes = json::Value::array();
  for (const auto m : supported_modes()) {
    modes.push_back(json::Value::string(model::to_string(m)));
  }
  v.set("supported_modes", std::move(modes));
  json::Value th = json::Value::array();
  for (const auto& t : thresholds()) th.push_back(t.to_json());
  v.set("thresholds", std::move(th));
  json::Value fp = json::Value::array();
  for (const auto& s : known_false_positives()) fp.push_back(json::Value::string(s));
  v.set("known_false_positives", std::move(fp));
  return v;
}

std::string make_fingerprint(const std::string& rule_id,
                             const std::string& rule_version,
                             const std::string& scope_key,
                             const std::string& discriminator) {
  // FNV-1a over the stable parts only. Timestamps are excluded so that
  // re-analysing the same input yields the same fingerprint.
  const std::string material =
      rule_id + "\x1f" + rule_version + "\x1f" + scope_key + "\x1f" + discriminator;
  std::uint64_t h = 1469598103934665603ull;
  for (const char ch : material) {
    h ^= static_cast<unsigned char>(ch);
    h *= 1099511628211ull;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return rule_id + "-" + buf;
}

}  // namespace mpi::rules
