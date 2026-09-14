// Rule contract (spec section 10.3).
//
// Each detector declares its prerequisites, supported modes, minimum coverage,
// thresholds and their origin, and its fixtures. A rule that cannot run says
// so; it never returns "no issue found".
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/rules/regression_input.hpp"
#include "core/symbols/symbol_service.hpp"
#include "core/util/cancel.hpp"

namespace mpi::rules {

// Where a number came from. A project budget, a platform deadline, and an
// empirical baseline are different claims and are never conflated.
enum class ThresholdOrigin {
  kProjectBudget,
  kPlatformDeadlineObserved,  // derived from the observed refresh rate
  kEmpiricalBaseline,
  kConfigurableHeuristic,     // our initial default; explicitly not a standard
};
const char* to_string(ThresholdOrigin o);

struct ThresholdSpec {
  std::string name;
  double value = 0.0;
  std::string unit;
  ThresholdOrigin origin = ThresholdOrigin::kConfigurableHeuristic;
  std::string rationale;
  json::Value to_json() const;
};

// What a rule needs before it is allowed to run.
struct Prerequisite {
  std::string id;
  std::string description;
};

struct RuleContext {
  const model::NormalizedTrace* trace = nullptr;
  // Set only when the analysis is over a baseline/candidate pair. DET-08 is
  // the one detector whose evidence is two run sets rather than one capture,
  // and it skips -- saying so -- when this is absent.
  const RegressionInput* regression = nullptr;
  const symbols::SymbolService* symbol_service = nullptr;
  model::MeasurementMode mode = model::MeasurementMode::kUnknownLimited;
  model::Eligibility eligibility;
  CancellationToken cancel;
  // Project-supplied threshold overrides, keyed by "RULE_ID.threshold_name".
  std::vector<std::pair<std::string, double>> threshold_overrides;

  std::optional<double> override_for(const std::string& key) const;
};

struct RuleOutput {
  std::vector<model::Issue> issues;
  model::RuleRunRecord record;
  // Attribution is produced even when no issue crosses a threshold: the
  // original total next to its attributed subset is worth reporting on its own
  // (spec section 9 rule 9).
  std::vector<model::AttributionReport> attribution;
};

class Rule {
 public:
  virtual ~Rule() = default;
  virtual std::string id() const = 0;
  virtual std::string version() const = 0;
  virtual std::string category() const = 0;
  virtual std::string title() const = 0;
  // Milestone at which this rule is delivered: "M1", "M4", "M5".
  virtual std::string delivery_phase() const = 0;
  virtual std::vector<Prerequisite> prerequisites() const = 0;
  virtual std::vector<model::MeasurementMode> supported_modes() const;
  virtual std::vector<ThresholdSpec> thresholds() const { return {}; }
  // Known false positives, surfaced in the report so a reader can discount
  // the finding themselves (spec section 10.3).
  virtual std::vector<std::string> known_false_positives() const { return {}; }

  // True for the rules whose evidence is a baseline/candidate pair rather
  // than a capture. They are the only ones `analyze_comparison` runs, because
  // a comparison has no trace for the others to read.
  virtual bool uses_comparison_input() const { return false; }

  // Returns the reasons this rule cannot run, or an empty vector when it can.
  virtual std::vector<std::string> unmet_prerequisites(
      const RuleContext& ctx) const = 0;

  // Runs the detector. Called only when unmet_prerequisites() is empty.
  virtual void evaluate(const RuleContext& ctx, RuleOutput& out) const = 0;

  // Wraps evaluate() with the skip bookkeeping every rule needs.
  RuleOutput run(const RuleContext& ctx) const;

  json::Value describe() const;
};

using RulePtr = std::shared_ptr<Rule>;

// An interval from a producer's own clock, placed on the capture's timeline.
//
// A marker is stamped by the app. Reporting its raw numbers as an issue's
// interval sends the UI to the wrong place in the capture -- spec section 13
// requires clicking an issue to focus its actual evidence -- so a rule maps
// the interval when a *measured* mapping exists and says so when it does not.
// Nothing is ever shifted by an assumed offset.
struct MappedInterval {
  model::TimeNs start_ns = 0;
  model::TimeNs end_ns = 0;
  bool mapped = false;
  // The clock the returned values are on.
  std::string domain;
};
MappedInterval map_producer_interval(const model::NormalizedTrace& trace,
                                     const std::string& producer_domain,
                                     model::TimeNs start_ns,
                                     model::TimeNs end_ns);

// Stable fingerprint so re-analysing the same input yields the same issue
// identity (spec section 15: stable issue fingerprints).
std::string make_fingerprint(const std::string& rule_id,
                             const std::string& rule_version,
                             const std::string& scope_key,
                             const std::string& discriminator);

}  // namespace mpi::rules
