#pragma once

#include <string>
#include <vector>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/rules/rule.hpp"
#include "core/symbols/symbol_service.hpp"
#include "core/util/cancel.hpp"

namespace mpi::rules {

struct EngineOptions {
  model::MeasurementMode mode = model::MeasurementMode::kDiagnostic;
  // "RULE_ID.threshold_name" -> value.
  std::vector<std::pair<std::string, double>> threshold_overrides;
  // Rule ids to suppress, with an audited reason and expiry (spec section 13).
  struct Suppression {
    std::string rule_id;
    std::string fingerprint;  // empty means every issue from the rule
    std::string reason;
    std::string expiry;
    std::string author;
  };
  std::vector<Suppression> suppressions;
  CancellationToken cancel;
};

std::string engine_version();

// Runs every registered rule against the trace. Rules that cannot run appear
// in `rule_runs` as skipped with reasons; they never silently vanish.
model::AnalysisResult analyze(const model::NormalizedTrace& trace,
                              const symbols::SymbolService& symbols,
                              const EngineOptions& opts);

// Runs the rules whose evidence is a baseline/candidate pair. Only those: a
// comparison carries no capture, so asking a frame rule to explain it would
// produce a skip about missing frames that tells the reader nothing about the
// comparison they asked for.
model::AnalysisResult analyze_comparison(const RegressionInput& comparison,
                                         const EngineOptions& opts);

}  // namespace mpi::rules
