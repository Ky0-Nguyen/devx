// Issue contract (spec section 10.1).
//
// Detection status and cause status are separate axes: an observed symptom
// with an unknown cause is the normal, honest output. Severity is not
// causal confidence (H06), and "no issue found" is not "the detector did
// not run" (H11).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model/build.hpp"
#include "core/model/metric.hpp"
#include "core/util/json.hpp"

namespace mpi::model {

enum class DetectionStatus { kObserved, kSuspected, kInconclusive };
const char* to_string(DetectionStatus s);

enum class CauseStatus {
  kUnknown,
  kCandidate,
  kSupported,
  kExperimentallyVerified,
};
const char* to_string(CauseStatus s);

// Impact-ordered, and explicitly not a confidence value.
enum class Severity { kInfo, kLow, kMedium, kHigh };
const char* to_string(Severity s);

// A pointer back into captured data. Every evidence ref must resolve (H07).
struct EvidenceRef {
  std::string kind;  // "event" | "metric" | "coverage" | "sample_set" | "marker"
  std::string id;    // event_id / metric name / gap id
  std::optional<TimeNs> start_ns;
  std::optional<TimeNs> end_ns;
  std::string note;
  bool synthetic = false;  // fixture-sourced evidence is labelled (H15)
  json::Value to_json() const;
};

struct SourceLocation {
  std::string file;
  std::optional<int> line;
  std::optional<int> column;
  std::string symbol;
  // exact_build_match | partial | unresolved | mismatch | unavailable
  std::string symbol_status = "unavailable";
  std::string note;
  // A non-exact match must never be opened as if it were the right revision.
  bool safe_to_open() const { return symbol_status == "exact_build_match"; }
  json::Value to_json() const;
};

struct CandidateStack {
  std::vector<std::string> frames;  // outermost first
  std::optional<double> sample_share;  // inclusive share, not a disjoint cost
  bool inclusive = true;
  std::vector<SourceLocation> locations;
  std::string note;
  json::Value to_json() const;
};

struct Issue {
  std::string issue_id;
  std::string rule_id;
  std::string rule_version;
  std::string session_id;
  std::string category;
  std::string title;

  Severity severity = Severity::kInfo;
  std::string severity_rationale;

  DetectionStatus detection_status = DetectionStatus::kInconclusive;
  CauseStatus cause_status = CauseStatus::kUnknown;

  MeasurementMode mode = MeasurementMode::kUnknownLimited;
  Eligibility eligibility;

  TimeNs start_ns = 0;
  TimeNs end_ns = 0;
  std::string process_instance_id;
  std::string thread_instance_id;

  // Only when actually observed via a marker; never guessed from a function
  // name (spec section 11).
  std::string screen;
  std::string interaction;

  std::vector<Metric> metrics;
  // The threshold that fired, plus where the number came from: a project
  // budget, a platform deadline, and an empirical baseline are different
  // things (spec section 10.3).
  std::string threshold_expression;
  std::string threshold_origin;
  std::optional<double> baseline_value;

  std::vector<EvidenceRef> evidence;
  std::vector<CandidateStack> candidate_stacks;
  std::string symbol_status = "unavailable";

  // What would have to be true for the cause to be established.
  std::vector<std::string> missing_evidence;
  std::vector<std::string> alternative_explanations;
  std::vector<std::string> suggested_verification;
  std::vector<std::string> proposed_remediation;
  // Prose basis for the confidence, never an uncalibrated number (H16).
  std::string confidence_basis;

  std::int64_t occurrence_count = 1;
  std::string fingerprint;
  std::vector<QualityFlag> quality_flags;

  bool suppressed = false;
  std::string suppression_reason;
  std::string suppression_expiry;
  std::string suppression_author;

  json::Value to_json() const;
};

// Why a rule produced nothing. "Did not run" must be distinguishable from
// "ran and found nothing" (spec H05, H11).
enum class RuleOutcome { kRanFoundIssues, kRanFoundNothing, kSkipped };
const char* to_string(RuleOutcome o);

struct RuleRunRecord {
  std::string rule_id;
  std::string rule_version;
  RuleOutcome outcome = RuleOutcome::kSkipped;
  // Populated when outcome == kSkipped.
  std::vector<std::string> skipped_reasons;
  std::vector<std::string> prerequisites;
  std::int64_t issues_emitted = 0;
  std::optional<double> coverage_fraction;
  json::Value to_json() const;
};

struct AnalysisResult {
  std::string session_id;
  std::string engine_version;
  std::string ruleset_version;
  std::string analyzed_at;
  MeasurementMode mode = MeasurementMode::kUnknownLimited;
  Eligibility eligibility;
  std::vector<Issue> issues;
  std::vector<RuleRunRecord> rule_runs;
  std::vector<AttributionReport> attribution;
  std::vector<Coverage> coverage;
  std::vector<std::string> data_quality_notes;
  bool contains_synthetic_data = false;

  json::Value to_json() const;
};

}  // namespace mpi::model
