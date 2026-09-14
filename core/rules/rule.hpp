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

// Stable fingerprint so re-analysing the same input yields the same issue
// identity (spec section 15: stable issue fingerprints).
std::string make_fingerprint(const std::string& rule_id,
                             const std::string& rule_version,
                             const std::string& scope_key,
                             const std::string& discriminator);

}  // namespace mpi::rules
