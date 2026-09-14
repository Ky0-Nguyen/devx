#pragma once

#include <string>
#include <vector>

#include "core/rules/rule.hpp"

namespace mpi::rules {

// Every detector in the specification's catalog is registered, including the
// ones not yet implemented. That is deliberate: spec H05 and H11 require an
// unsupported rule to be *reported as skipped*, which is only possible if the
// engine knows it exists.
std::vector<RulePtr> all_rules();

// Ruleset version, bumped whenever a rule or threshold changes. Recorded in
// every report (spec H09).
std::string ruleset_version();

}  // namespace mpi::rules
