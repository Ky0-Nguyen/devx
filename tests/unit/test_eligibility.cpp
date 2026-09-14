#include <sstream>

#include "core/model/build.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::model;

namespace {

BuildFact boolean_fact(const char* key, Tri value, FactSource source) {
  BuildFact f;
  f.key = key;
  f.boolean_value = value;
  f.source = source;
  f.value = to_string(value);
  return f;
}

// A build profile with every benchmark-relevant fact set favourably.
BuildProfile clean_release_profile() {
  BuildProfile p;
  p.upsert(boolean_fact("native.optimized", Tri::kTrue, FactSource::kBuildPluginManifest));
  p.upsert(boolean_fact("js.__DEV__", Tri::kFalse, FactSource::kRuntimeSdk));
  p.upsert(boolean_fact("native.debuggable", Tri::kFalse, FactSource::kDeviceProvider));
  p.upsert(boolean_fact("runtime.debugger_attached", Tri::kFalse, FactSource::kRuntimeSdk));
  p.upsert(boolean_fact("runtime.sanitizers_enabled", Tri::kFalse, FactSource::kBuildPluginManifest));
  p.upsert(boolean_fact("runtime.heavy_instrumentation", Tri::kFalse, FactSource::kBuildPluginManifest));
  p.upsert(boolean_fact("runtime.remote_js_debugging", Tri::kFalse, FactSource::kRuntimeSdk));
  BuildFact form;
  form.key = "device.form";
  form.value = "physical";
  form.source = FactSource::kDeviceProvider;
  p.upsert(std::move(form));
  return p;
}

bool has_reason(const Eligibility& e, const std::string& needle) {
  for (const auto& r : e.reasons) {
    if (r.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(clean_optimized_build_is_benchmark_eligible, {"C04"}) {
  const auto e = evaluate_eligibility(clean_release_profile(),
                                      MeasurementMode::kBenchmark);
  MPI_CHECK_MSG(e.status == EligibilityStatus::kEligible,
                "expected eligible, reasons: " +
                    (e.reasons.empty() ? std::string("(none)") : e.reasons[0]));
  MPI_CHECK(e.certified_benchmark());
}

MPI_TEST(diagnostic_mode_never_certifies_release, {"C01", "F14", "I16"}) {
  // Even with a perfect release build, a diagnostic session is not a pass.
  const auto e = evaluate_eligibility(clean_release_profile(),
                                      MeasurementMode::kDiagnostic);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(!e.certified_benchmark());
  MPI_CHECK(has_reason(e, "mode_is_not_benchmark"));
}

MPI_TEST(js_dev_mode_makes_benchmark_ineligible, {"C01", "C03"}) {
  auto p = clean_release_profile();
  p.upsert(boolean_fact("js.__DEV__", Tri::kTrue, FactSource::kRuntimeSdk));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "js_dev_mode_enabled"));
}

MPI_TEST(unknown_fact_yields_insufficient_evidence_not_a_pass, {"C18"}) {
  auto p = clean_release_profile();
  // Remove the optimization fact entirely: we never looked.
  BuildProfile stripped;
  for (const auto& f : p.facts) {
    if (f.key != "native.optimized") stripped.upsert(f);
  }
  const auto e = evaluate_eligibility(stripped, MeasurementMode::kBenchmark);
  MPI_CHECK_MSG(e.status == EligibilityStatus::kInsufficientEvidence,
                "an unknown must not become a pass");
  MPI_CHECK(has_reason(e, "native_optimization_unknown"));
  MPI_CHECK(!e.certified_benchmark());
}

MPI_TEST(eligibility_reports_every_reason_not_just_the_first, {"C19"}) {
  BuildProfile p;  // nothing known at all
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK_MSG(e.reasons.size() >= 7,
                "expected a reason per unknown dimension, got " +
                    std::to_string(e.reasons.size()));
}

MPI_TEST(debugger_attached_invalidates_benchmark, {"C07", "C08"}) {
  auto p = clean_release_profile();
  p.upsert(boolean_fact("runtime.debugger_attached", Tri::kTrue, FactSource::kRuntimeSdk));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "debugger_attached"));
}

MPI_TEST(debuggable_without_attached_debugger_is_still_ineligible, {"C06"}) {
  // Permission to debug is not debugger attachment, but it still means the
  // build is not the one users run.
  auto p = clean_release_profile();
  p.upsert(boolean_fact("native.debuggable", Tri::kTrue, FactSource::kDeviceProvider));
  p.upsert(boolean_fact("runtime.debugger_attached", Tri::kFalse, FactSource::kRuntimeSdk));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "debuggable_build"));
  MPI_CHECK_MSG(!has_reason(e, "debugger_attached"),
                "debuggable must not be reported as an attached debugger");
}

MPI_TEST(sanitizers_invalidate_benchmark, {"C13"}) {
  auto p = clean_release_profile();
  p.upsert(boolean_fact("runtime.sanitizers_enabled", Tri::kTrue,
                        FactSource::kBuildPluginManifest));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "sanitizers_enabled"));
}

MPI_TEST(remote_js_execution_invalidates_benchmark, {"G11"}) {
  auto p = clean_release_profile();
  p.upsert(boolean_fact("runtime.remote_js_debugging", Tri::kTrue,
                        FactSource::kRuntimeSdk));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "remote_js_execution"));
}

MPI_TEST(simulator_is_not_benchmark_eligible, {"J18", "I02"}) {
  auto p = clean_release_profile();
  BuildFact form;
  form.key = "device.form";
  form.value = "simulator";
  form.source = FactSource::kDeviceProvider;
  p.upsert(std::move(form));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(e.status == EligibilityStatus::kIneligible);
  MPI_CHECK(has_reason(e, "simulator"));
}

MPI_TEST(conflicting_facts_are_recorded_and_block_certification, {"C10"}) {
  BuildProfile p = clean_release_profile();
  // The runtime disagrees with the build manifest.
  p.upsert(boolean_fact("native.optimized", Tri::kFalse, FactSource::kRuntimeSdk));
  MPI_CHECK_MSG(!p.conflicts.empty(), "a disagreement must be recorded");
  MPI_CHECK_EQ(p.conflicts[0].key, std::string("native.optimized"));
  const auto e = evaluate_eligibility(p, MeasurementMode::kBenchmark);
  MPI_CHECK(!e.certified_benchmark());
}

MPI_TEST(stronger_source_wins_but_conflict_is_kept, {"C10"}) {
  BuildProfile p;
  p.upsert(boolean_fact("native.optimized", Tri::kFalse, FactSource::kInferred));
  p.upsert(boolean_fact("native.optimized", Tri::kTrue,
                        FactSource::kBuildPluginManifest));
  MPI_CHECK(p.boolean("native.optimized") == Tri::kTrue);
  MPI_CHECK_EQ(p.conflicts.size(), static_cast<std::size_t>(1));
}

MPI_TEST(user_override_is_audited_and_never_certifies, {"C19", "H10"}) {
  auto e = evaluate_eligibility(clean_release_profile(), MeasurementMode::kBenchmark);
  MPI_CHECK(e.certified_benchmark());
  e.user_override = true;
  e.user_override_note = "exploratory comparison requested by the operator";
  MPI_CHECK_MSG(!e.certified_benchmark(),
                "an override permits exploration; it cannot certify");
  const auto j = e.to_json();
  MPI_CHECK_EQ(j.find("user_override")->as_bool(), true);
  MPI_CHECK(!j.find("user_override_note")->is_null());
  MPI_CHECK_EQ(j.find("certified_benchmark")->as_bool(), false);
}

MPI_TEST(eligibility_never_implies_zero_overhead, {"section-7.3"}) {
  const auto e = evaluate_eligibility(clean_release_profile(),
                                      MeasurementMode::kBenchmark);
  MPI_CHECK_EQ(e.to_json().find("implies_zero_profiler_overhead")->as_bool(), false);
}

MPI_TEST(missing_fact_returns_unknown_not_false, {"C18"}) {
  BuildProfile p;
  MPI_CHECK(p.boolean("never.set") == Tri::kUnknown);
  MPI_CHECK(p.find("never.set") == nullptr);
  // And the JSON form keeps it null rather than false.
  BuildFact f;
  f.key = "never.set";
  f.boolean_value = Tri::kUnknown;
  MPI_CHECK(f.to_json().find("boolean_value")->is_null());
}
