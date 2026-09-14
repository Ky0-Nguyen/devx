#include "core/model/build.hpp"

namespace mpi::model {

const char* to_string(FactSource s) {
  switch (s) {
    case FactSource::kBuildPluginManifest: return "build_plugin_manifest";
    case FactSource::kRuntimeSdk: return "runtime_sdk";
    case FactSource::kDeviceProvider: return "device_provider";
    case FactSource::kHostToolchain: return "host_toolchain";
    case FactSource::kUserAsserted: return "user_asserted";
    case FactSource::kInferred: return "inferred";
    case FactSource::kUnknown: return "unknown";
  }
  return "unknown";
}

const char* to_string(Tri t) {
  switch (t) {
    case Tri::kTrue: return "true";
    case Tri::kFalse: return "false";
    case Tri::kUnknown: return "unknown";
  }
  return "unknown";
}

json::Value BuildFact::to_json() const {
  json::Value v = json::Value::object();
  v.set("key", json::Value::string(key));
  v.set("value", value.empty() ? json::Value::null() : json::Value::string(value));
  v.set("boolean_value", boolean_value == Tri::kUnknown
                             ? json::Value::null()
                             : json::Value::boolean(boolean_value == Tri::kTrue));
  v.set("tri_state", json::Value::string(to_string(boolean_value)));
  v.set("source", json::Value::string(to_string(source)));
  v.set("observed_at", json::Value::string(observed_at));
  v.set("basis", json::Value::string(basis));
  return v;
}

json::Value FactConflict::to_json() const {
  json::Value v = json::Value::object();
  v.set("key", json::Value::string(key));
  v.set("a", a.to_json());
  v.set("b", b.to_json());
  return v;
}

json::Value SymbolBinding::to_json() const {
  json::Value v = json::Value::object();
  v.set("status", json::Value::string(status));
  v.set("kind", json::Value::string(kind));
  v.set("expected_id", expected_id.empty() ? json::Value::null()
                                           : json::Value::string(expected_id));
  v.set("actual_id",
        actual_id.empty() ? json::Value::null() : json::Value::string(actual_id));
  v.set("artifact_path", artifact_path.empty()
                             ? json::Value::null()
                             : json::Value::string(artifact_path));
  v.set("note", json::Value::string(note));
  return v;
}

const BuildFact* BuildProfile::find(const std::string& key) const {
  for (const auto& f : facts) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

Tri BuildProfile::boolean(const std::string& key) const {
  const BuildFact* f = find(key);
  return f ? f->boolean_value : Tri::kUnknown;
}

void BuildProfile::upsert(BuildFact f) {
  for (auto& existing : facts) {
    if (existing.key != f.key) continue;
    // Same key, different answer: record the conflict instead of overwriting.
    const bool differs = existing.value != f.value ||
                         existing.boolean_value != f.boolean_value;
    if (differs && existing.known() && f.known()) {
      conflicts.push_back(FactConflict{f.key, existing, f});
    }
    // Keep the stronger source as the primary value; the conflict is recorded.
    if (static_cast<int>(f.source) <= static_cast<int>(existing.source)) {
      existing = std::move(f);
    }
    return;
  }
  facts.push_back(std::move(f));
}

json::Value BuildProfile::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  json::Value f = json::Value::array();
  for (const auto& x : facts) f.push_back(x.to_json());
  v.set("facts", std::move(f));
  json::Value c = json::Value::array();
  for (const auto& x : conflicts) c.push_back(x.to_json());
  v.set("conflicts", std::move(c));
  json::Value s = json::Value::array();
  for (const auto& x : symbol_bindings) s.push_back(x.to_json());
  v.set("symbol_bindings", std::move(s));
  return v;
}

const char* to_string(MeasurementMode m) {
  switch (m) {
    case MeasurementMode::kDiagnostic: return "diagnostic";
    case MeasurementMode::kBenchmark: return "benchmark";
    case MeasurementMode::kUnknownLimited: return "unknown_limited";
  }
  return "unknown_limited";
}

MeasurementMode measurement_mode_from_string(const std::string& s) {
  if (s == "diagnostic") return MeasurementMode::kDiagnostic;
  if (s == "benchmark") return MeasurementMode::kBenchmark;
  return MeasurementMode::kUnknownLimited;
}

const char* to_string(EligibilityStatus s) {
  switch (s) {
    case EligibilityStatus::kEligible: return "eligible";
    case EligibilityStatus::kIneligible: return "ineligible";
    case EligibilityStatus::kInsufficientEvidence: return "insufficient_evidence";
  }
  return "insufficient_evidence";
}

json::Value Eligibility::to_json() const {
  json::Value v = json::Value::object();
  v.set("status", json::Value::string(to_string(status)));
  json::Value r = json::Value::array();
  for (const auto& x : reasons) r.push_back(json::Value::string(x));
  v.set("reasons", std::move(r));
  v.set("user_override", json::Value::boolean(user_override));
  v.set("user_override_note", user_override_note.empty()
                                  ? json::Value::null()
                                  : json::Value::string(user_override_note));
  v.set("certified_benchmark", json::Value::boolean(certified_benchmark()));
  // Spec section 7.3: eligibility never implies zero profiler overhead.
  v.set("implies_zero_profiler_overhead", json::Value::boolean(false));
  return v;
}

Eligibility evaluate_eligibility(const BuildProfile& profile,
                                 MeasurementMode requested_mode) {
  Eligibility e;
  if (requested_mode != MeasurementMode::kBenchmark) {
    // Diagnostic and unknown/limited modes are never a benchmark pass.
    e.status = EligibilityStatus::kIneligible;
    e.reasons.push_back(std::string("mode_is_not_benchmark:") +
                        to_string(requested_mode));
    return e;
  }

  bool blocking = false;
  bool unknown_blocking = false;

  // Each entry: fact key, the value that makes a benchmark invalid, and the
  // reason string. An unknown yields insufficient_evidence, not a pass.
  struct Check {
    const char* key;
    Tri invalid_when;
    const char* reason_when_invalid;
    const char* reason_when_unknown;
  };
  static constexpr Check kChecks[] = {
      {"native.optimized", Tri::kFalse, "native_build_not_optimized",
       "native_optimization_unknown"},
      {"js.__DEV__", Tri::kTrue, "js_dev_mode_enabled", "js_dev_mode_unknown"},
      {"native.debuggable", Tri::kTrue, "debuggable_build",
       "debuggable_state_unknown"},
      {"runtime.debugger_attached", Tri::kTrue, "debugger_attached",
       "debugger_attachment_unknown"},
      {"runtime.sanitizers_enabled", Tri::kTrue, "sanitizers_enabled",
       "sanitizer_state_unknown"},
      {"runtime.heavy_instrumentation", Tri::kTrue, "heavy_instrumentation_enabled",
       "instrumentation_state_unknown"},
      {"runtime.remote_js_debugging", Tri::kTrue, "remote_js_execution",
       "remote_js_debugging_unknown"},
  };

  for (const auto& c : kChecks) {
    const Tri actual = profile.boolean(c.key);
    if (actual == Tri::kUnknown) {
      unknown_blocking = true;
      e.reasons.push_back(c.reason_when_unknown);
    } else if (actual == c.invalid_when) {
      blocking = true;
      e.reasons.push_back(c.reason_when_invalid);
    }
  }

  // A simulator or emulator run is a valid benchmark only against another run
  // of the same form; it is never comparable to a physical device (spec 4.1).
  if (const BuildFact* form = profile.find("device.form")) {
    if (form->value == "simulator" || form->value == "emulator") {
      e.reasons.push_back("device_form_is_" + form->value +
                          "_not_comparable_to_physical_device");
      blocking = true;
    }
  } else {
    unknown_blocking = true;
    e.reasons.push_back("device_form_unknown");
  }

  // Disagreeing facts cannot certify anything.
  for (const auto& c : profile.conflicts) {
    unknown_blocking = true;
    e.reasons.push_back("conflicting_build_fact:" + c.key);
  }

  if (blocking) {
    e.status = EligibilityStatus::kIneligible;
  } else if (unknown_blocking) {
    e.status = EligibilityStatus::kInsufficientEvidence;
  } else {
    e.status = EligibilityStatus::kEligible;
  }
  return e;
}

}  // namespace mpi::model
