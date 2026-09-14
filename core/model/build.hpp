// Build detection and measurement eligibility (spec section 7).
//
// The dimensions here are independent. A configuration named "Release" does
// not prove production-like behavior; permission to debug is not debugger
// attachment; and an unknown boolean stays unknown rather than collapsing to
// false (spec C18).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::model {

// Where a fact came from. Confidence follows from the source, not from a
// hand-assigned number (spec H16 forbids invented numeric confidence).
enum class FactSource {
  kBuildPluginManifest,  // emitted at build time, strongest
  kRuntimeSdk,           // reported by the in-app SDK at runtime
  kDeviceProvider,       // read from the device via adb / devicectl
  kHostToolchain,        // read from the host toolchain
  kUserAsserted,         // typed by the user; auditable, never certified
  kInferred,             // derived from other facts; weakest
  kUnknown,
};
const char* to_string(FactSource s);

// Tri-state. kUnknown is a first-class answer, never rendered as false.
enum class Tri { kTrue, kFalse, kUnknown };
const char* to_string(Tri t);

struct BuildFact {
  std::string key;    // e.g. "native.optimized", "js.__DEV__"
  std::string value;  // canonical string form; "" only when unknown
  Tri boolean_value = Tri::kUnknown;  // set when the fact is boolean
  FactSource source = FactSource::kUnknown;
  std::string observed_at;
  std::string basis;  // what was actually read, verbatim where short

  bool known() const { return source != FactSource::kUnknown; }
  json::Value to_json() const;
};

// Two facts that disagree. Spec section 7.1: conflicts are visible, not
// silently resolved in favour of the "better" source.
struct FactConflict {
  std::string key;
  BuildFact a;
  BuildFact b;
  json::Value to_json() const;
};

struct SymbolBinding {
  // exact_build_match | partial | unresolved | mismatch | unavailable
  std::string status = "unavailable";
  std::string kind;        // "native_dsym", "native_build_id", "r8_map", "js_source_map"
  std::string expected_id; // build id / UUID / bundle hash we need
  std::string actual_id;   // what the supplied artifact carries
  std::string artifact_path;
  std::string note;
  bool exact() const { return status == "exact_build_match"; }
  json::Value to_json() const;
};

struct BuildProfile {
  std::vector<BuildFact> facts;
  std::vector<FactConflict> conflicts;
  std::vector<SymbolBinding> symbol_bindings;

  const BuildFact* find(const std::string& key) const;
  // Returns kUnknown when the fact is absent: callers cannot accidentally
  // treat "we never looked" as "no".
  Tri boolean(const std::string& key) const;
  void upsert(BuildFact f);

  json::Value to_json() const;
};

enum class MeasurementMode {
  kDiagnostic,     // investigate debug behavior; never release certification
  kBenchmark,      // compare production-like measurements
  kUnknownLimited,  // incomplete capability/build info; no automatic pass
};
const char* to_string(MeasurementMode m);
MeasurementMode measurement_mode_from_string(const std::string& s);

enum class EligibilityStatus { kEligible, kIneligible, kInsufficientEvidence };
const char* to_string(EligibilityStatus s);

struct Eligibility {
  EligibilityStatus status = EligibilityStatus::kInsufficientEvidence;
  // Every reason, not the first one (spec section 7.3).
  std::vector<std::string> reasons;
  // Set when a user forced an exploratory comparison. Auditable; it records
  // the override, it does not erase the invalidity (spec C19).
  bool user_override = false;
  std::string user_override_note;

  // True only for a clean benchmark verdict; an override never makes this true.
  bool certified_benchmark() const {
    return status == EligibilityStatus::kEligible && !user_override;
  }
  json::Value to_json() const;
};

// Decides benchmark eligibility from the build profile. Deliberately
// conservative: any unknown that could change timing yields
// kInsufficientEvidence rather than kEligible.
Eligibility evaluate_eligibility(const BuildProfile& profile,
                                 MeasurementMode requested_mode);

}  // namespace mpi::model
