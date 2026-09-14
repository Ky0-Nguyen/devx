#include "core/model/issue.hpp"

namespace mpi::model {
namespace {
json::Value strings(const std::vector<std::string>& in) {
  json::Value a = json::Value::array();
  for (const auto& s : in) a.push_back(json::Value::string(s));
  return a;
}
}  // namespace

const char* to_string(DetectionStatus s) {
  switch (s) {
    case DetectionStatus::kObserved: return "observed";
    case DetectionStatus::kSuspected: return "suspected";
    case DetectionStatus::kInconclusive: return "inconclusive";
  }
  return "inconclusive";
}

const char* to_string(CauseStatus s) {
  switch (s) {
    case CauseStatus::kUnknown: return "unknown";
    case CauseStatus::kCandidate: return "candidate";
    case CauseStatus::kSupported: return "supported";
    case CauseStatus::kExperimentallyVerified: return "experimentally_verified";
  }
  return "unknown";
}

const char* to_string(Severity s) {
  switch (s) {
    case Severity::kInfo: return "info";
    case Severity::kLow: return "low";
    case Severity::kMedium: return "medium";
    case Severity::kHigh: return "high";
  }
  return "info";
}

json::Value EvidenceRef::to_json() const {
  json::Value v = json::Value::object();
  v.set("kind", json::Value::string(kind));
  v.set("id", json::Value::string(id));
  v.set("start_ns",
        start_ns.has_value() ? json::Value::integer(*start_ns) : json::Value::null());
  v.set("end_ns",
        end_ns.has_value() ? json::Value::integer(*end_ns) : json::Value::null());
  v.set("note", json::Value::string(note));
  v.set("synthetic", json::Value::boolean(synthetic));
  return v;
}

json::Value SourceLocation::to_json() const {
  json::Value v = json::Value::object();
  v.set("file", file.empty() ? json::Value::null() : json::Value::string(file));
  v.set("line", line.has_value() ? json::Value::integer(*line) : json::Value::null());
  v.set("column",
        column.has_value() ? json::Value::integer(*column) : json::Value::null());
  v.set("symbol", symbol.empty() ? json::Value::null() : json::Value::string(symbol));
  v.set("symbol_status", json::Value::string(symbol_status));
  v.set("note", json::Value::string(note));
  // The UI must not navigate on a non-exact match (spec section 11).
  v.set("safe_to_open", json::Value::boolean(safe_to_open()));
  return v;
}

json::Value CandidateStack::to_json() const {
  json::Value v = json::Value::object();
  v.set("frames", strings(frames));
  v.set("sample_share", sample_share.has_value()
                            ? json::Value::number(*sample_share)
                            : json::Value::null());
  v.set("inclusive", json::Value::boolean(inclusive));
  json::Value locs = json::Value::array();
  for (const auto& l : locations) locs.push_back(l.to_json());
  v.set("locations", std::move(locs));
  v.set("note", json::Value::string(note));
  // Inclusive shares are not disjoint costs and must not be summed (F17).
  v.set("summable_as_disjoint_cost", json::Value::boolean(!inclusive));
  return v;
}

json::Value Issue::to_json() const {
  json::Value v = json::Value::object();
  v.set("issue_id", json::Value::string(issue_id));
  v.set("rule_id", json::Value::string(rule_id));
  v.set("rule_version", json::Value::string(rule_version));
  v.set("session_id", json::Value::string(session_id));
  v.set("category", json::Value::string(category));
  v.set("title", json::Value::string(title));
  v.set("severity", json::Value::string(to_string(severity)));
  v.set("severity_rationale", json::Value::string(severity_rationale));
  v.set("detection_status", json::Value::string(to_string(detection_status)));
  v.set("cause_status", json::Value::string(to_string(cause_status)));
  v.set("measurement_mode", json::Value::string(to_string(mode)));
  v.set("benchmark_eligibility", eligibility.to_json());
  v.set("start_ns", json::Value::integer(start_ns));
  v.set("end_ns", json::Value::integer(end_ns));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("thread_instance_id", thread_instance_id.empty()
                                  ? json::Value::null()
                                  : json::Value::string(thread_instance_id));
  // Null rather than guessed: screens come from markers only.
  v.set("screen", screen.empty() ? json::Value::null() : json::Value::string(screen));
  v.set("interaction", interaction.empty() ? json::Value::null()
                                           : json::Value::string(interaction));
  json::Value ms = json::Value::array();
  for (const auto& m : metrics) ms.push_back(m.to_json());
  v.set("metrics", std::move(ms));
  v.set("threshold_expression", json::Value::string(threshold_expression));
  v.set("threshold_origin", json::Value::string(threshold_origin));
  v.set("baseline_value", baseline_value.has_value()
                              ? json::Value::number(*baseline_value)
                              : json::Value::null());
  json::Value ev = json::Value::array();
  for (const auto& e : evidence) ev.push_back(e.to_json());
  v.set("evidence_refs", std::move(ev));
  json::Value st = json::Value::array();
  for (const auto& s : candidate_stacks) st.push_back(s.to_json());
  v.set("candidate_stacks", std::move(st));
  v.set("symbol_status", json::Value::string(symbol_status));
  v.set("missing_evidence", strings(missing_evidence));
  v.set("alternative_explanations", strings(alternative_explanations));
  v.set("suggested_verification", strings(suggested_verification));
  v.set("proposed_remediation", strings(proposed_remediation));
  v.set("confidence_basis", json::Value::string(confidence_basis));
  v.set("occurrence_count", json::Value::integer(occurrence_count));
  v.set("fingerprint", json::Value::string(fingerprint));
  json::Value fl = json::Value::array();
  for (const auto f : quality_flags) fl.push_back(json::Value::string(to_string(f)));
  v.set("quality_flags", std::move(fl));
  json::Value sup = json::Value::object();
  sup.set("suppressed", json::Value::boolean(suppressed));
  sup.set("reason", suppression_reason.empty()
                        ? json::Value::null()
                        : json::Value::string(suppression_reason));
  sup.set("expiry", suppression_expiry.empty()
                        ? json::Value::null()
                        : json::Value::string(suppression_expiry));
  sup.set("author", suppression_author.empty()
                        ? json::Value::null()
                        : json::Value::string(suppression_author));
  v.set("suppression", std::move(sup));
  return v;
}

const char* to_string(RuleOutcome o) {
  switch (o) {
    case RuleOutcome::kRanFoundIssues: return "ran_found_issues";
    case RuleOutcome::kRanFoundNothing: return "ran_found_nothing";
    case RuleOutcome::kSkipped: return "skipped";
  }
  return "skipped";
}

json::Value RuleRunRecord::to_json() const {
  json::Value v = json::Value::object();
  v.set("rule_id", json::Value::string(rule_id));
  v.set("rule_version", json::Value::string(rule_version));
  v.set("outcome", json::Value::string(to_string(outcome)));
  v.set("skipped_reasons", strings(skipped_reasons));
  v.set("prerequisites", strings(prerequisites));
  v.set("issues_emitted", json::Value::integer(issues_emitted));
  v.set("coverage_fraction", coverage_fraction.has_value()
                                 ? json::Value::number(*coverage_fraction)
                                 : json::Value::null());
  return v;
}

json::Value AnalysisResult::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  v.set("session_id", json::Value::string(session_id));
  v.set("engine_version", json::Value::string(engine_version));
  v.set("ruleset_version", json::Value::string(ruleset_version));
  v.set("analyzed_at", json::Value::string(analyzed_at));
  v.set("measurement_mode", json::Value::string(to_string(mode)));
  v.set("benchmark_eligibility", eligibility.to_json());
  json::Value is = json::Value::array();
  for (const auto& i : issues) is.push_back(i.to_json());
  v.set("issues", std::move(is));
  json::Value rr = json::Value::array();
  for (const auto& r : rule_runs) rr.push_back(r.to_json());
  v.set("rule_runs", std::move(rr));
  json::Value at = json::Value::array();
  for (const auto& a : attribution) at.push_back(a.to_json());
  v.set("attribution", std::move(at));
  json::Value cv = json::Value::array();
  for (const auto& c : coverage) cv.push_back(c.to_json());
  v.set("coverage", std::move(cv));
  v.set("data_quality_notes", strings(data_quality_notes));
  // Spec section 0.6 / H15: synthetic input is labelled at the top level too.
  v.set("synthetic", json::Value::boolean(contains_synthetic_data));
  return v;
}

}  // namespace mpi::model
