#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

std::string fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir ? dir : "fixtures") + "/" + rel;
}

model::NormalizedTrace load(const char* rel) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture(rel), ingest::ReadOptions{}, t, d);
  ingest::normalize(t);
  return t;
}

model::AnalysisResult run(const model::NormalizedTrace& t,
                          model::MeasurementMode mode =
                              model::MeasurementMode::kDiagnostic) {
  symbols::SymbolService s;
  rules::EngineOptions o;
  o.mode = mode;
  return rules::analyze(t, s, o);
}

const model::Issue* first(const model::AnalysisResult& r, const char* rule_id) {
  for (const auto& i : r.issues) {
    if (i.rule_id == rule_id) return &i;
  }
  return nullptr;
}

const model::RuleRunRecord* record_for(const model::AnalysisResult& r,
                                       const char* rule_id) {
  for (const auto& x : r.rule_runs) {
    if (x.rule_id == rule_id) return &x;
  }
  return nullptr;
}

// Substring search over a single string, for prose fields.
bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool contains(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v) {
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(every_catalog_detector_is_registered, {"H05", "H11"}) {
  const auto all = rules::all_rules();
  // All twelve detectors from the specification catalog must be present, so
  // that an unimplemented one can still be reported as skipped.
  for (const char* id : {"DET-01", "DET-02", "DET-03", "DET-04", "DET-05",
                         "DET-06", "DET-07", "DET-08", "DET-09", "DET-10",
                         "DET-11", "DET-12"}) {
    const bool found = std::any_of(all.begin(), all.end(),
                                   [&](const rules::RulePtr& r) {
                                     return r->id() == id;
                                   });
    MPI_CHECK_MSG(found, std::string("detector not registered: ") + id);
  }
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(12));
}

MPI_TEST(every_rule_declares_prerequisites_and_a_phase, {"section-10.3"}) {
  for (const auto& r : rules::all_rules()) {
    MPI_CHECK_MSG(!r->prerequisites().empty(),
                  r->id() + " declares no prerequisites");
    MPI_CHECK_MSG(!r->delivery_phase().empty(),
                  r->id() + " declares no delivery phase");
    MPI_CHECK_MSG(!r->category().empty(), r->id() + " declares no category");
  }
}

MPI_TEST(unimplemented_detectors_are_skipped_with_reasons, {"H05"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  for (const char* id : {"DET-03", "DET-05", "DET-06", "DET-07", "DET-08",
                         "DET-09", "DET-10", "DET-11"}) {
    const auto* rec = record_for(r, id);
    MPI_CHECK_MSG(rec != nullptr, std::string("no run record for ") + id);
    MPI_CHECK_MSG(rec->outcome == model::RuleOutcome::kSkipped,
                  std::string(id) + " should be skipped");
    MPI_CHECK_MSG(!rec->skipped_reasons.empty(),
                  std::string(id) + " must say why it was skipped");
    MPI_CHECK_MSG(contains(rec->skipped_reasons, "not implemented"),
                  std::string(id) + " must state that it is unimplemented");
  }
}

MPI_TEST(healthy_capture_runs_detectors_and_finds_nothing, {"H02", "H11"}) {
  const auto r = run(load("traces/negative-healthy.mpi.json"));
  MPI_CHECK_MSG(r.issues.empty(),
                "the negative fixture must produce no issue, got " +
                    std::to_string(r.issues.size()));
  // But the implemented detectors must have actually run.
  for (const char* id : {"DET-01", "DET-02", "DET-04", "DET-12"}) {
    const auto* rec = record_for(r, id);
    MPI_CHECK(rec != nullptr);
    MPI_CHECK_MSG(rec->outcome == model::RuleOutcome::kRanFoundNothing,
                  std::string(id) + " must have run, got outcome '" +
                      model::to_string(rec->outcome) + "'");
  }
}

MPI_TEST(det01_observed_requires_presentation_truth, {"DET-01", "E21", "H01"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* obs = first(r, "DET-01");
  MPI_CHECK(obs != nullptr);
  MPI_CHECK_MSG(obs->detection_status == model::DetectionStatus::kObserved,
                "real presentation timestamps support an observed detection");
  // Even an observed symptom leaves the cause unknown.
  MPI_CHECK(obs->cause_status == model::CauseStatus::kUnknown);
}

MPI_TEST(det01_proxy_source_downgrades_to_suspected, {"E21", "H03"}) {
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  const auto* proxy = first(r, "DET-01");
  MPI_CHECK(proxy != nullptr);
  MPI_CHECK_MSG(proxy->detection_status == model::DetectionStatus::kSuspected,
                "a display-callback proxy cannot support 'observed'");
  MPI_CHECK(contains(proxy->missing_evidence, "presentation timestamps"));
  MPI_CHECK(contains(proxy->alternative_explanations, "presented on time"));
}

MPI_TEST(det01_excludes_unjudgeable_frames_from_the_denominator, {"E21", "D11"}) {
  // The incomplete fixture has 40 frames, 15 of which have no presentation
  // timestamp. Those must be excluded, not counted as on-time.
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  const auto* i = first(r, "DET-01");
  MPI_CHECK(i != nullptr);
  const model::Metric* rate = nullptr;
  for (const auto& m : i->metrics) {
    if (m.name == "frames.missed_deadline_rate") rate = &m;
  }
  MPI_CHECK(rate != nullptr);
  MPI_CHECK_MSG(contains(rate->limitations, "excluded from the denominator"),
                "the exclusion must be stated as a limitation");
  MPI_CHECK_MSG(rate->value.has_value() && *rate->value > 0.4,
                "12 misses out of 25 judgeable frames is ~48%, not 30%");
}

MPI_TEST(det01_skips_when_no_frame_collector_ran, {"H03", "H11"}) {
  auto t = load("traces/positive-frames-js-cpu.mpi.json");
  t.frames.clear();
  const auto r = run(t);
  const auto* rec = record_for(r, "DET-01");
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK_MSG(contains(rec->skipped_reasons, "missing collector"),
                "an absent collector is not an absence of jank");
}

MPI_TEST(det01_skips_when_refresh_rate_unobserved, {"E01"}) {
  auto t = load("traces/positive-frames-js-cpu.mpi.json");
  t.refresh_intervals.clear();
  for (auto& f : t.frames) f.deadline_ns = std::nullopt;
  const auto r = run(t);
  const auto* rec = record_for(r, "DET-01");
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK_MSG(contains(rec->skipped_reasons, "16 ms"),
                "the skip reason should state there is no universal 16 ms rule");
}

MPI_TEST(det02_unmapped_clock_blocks_any_ui_claim, {"DET-02", "section-11"}) {
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  const auto* i = first(r, "DET-02");
  MPI_CHECK(i != nullptr);
  // The long task is measured, so detection is observed...
  MPI_CHECK(i->detection_status == model::DetectionStatus::kObserved);
  // ...but no UI-impact claim is derivable without a measured mapping.
  MPI_CHECK_MSG(i->cause_status == model::CauseStatus::kUnknown,
                "no mapped clock means no candidate cause");
  MPI_CHECK(contains(i->missing_evidence, "measured mapping"));
  MPI_CHECK(std::find(i->quality_flags.begin(), i->quality_flags.end(),
                      model::QualityFlag::kClockUnmapped) !=
            i->quality_flags.end());
}

MPI_TEST(det02_overlap_is_a_candidate_cause_never_proven, {"E20", "section-0.8"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-02");
  MPI_CHECK(i != nullptr);
  MPI_CHECK_MSG(i->cause_status == model::CauseStatus::kCandidate,
                "a temporal overlap is a candidate, not a demonstrated cause");
  MPI_CHECK_MSG(i->cause_status != model::CauseStatus::kSupported &&
                    i->cause_status != model::CauseStatus::kExperimentallyVerified,
                "correlation must never be promoted to proof");
  MPI_CHECK(contains(i->confidence_basis, "correlation"));
  MPI_CHECK(contains(i->alternative_explanations, "third factor"));
  MPI_CHECK(contains(i->suggested_verification, "single improved run is not proof"));
}

MPI_TEST(det02_threshold_is_labelled_a_heuristic_not_a_standard, {"section-10.3"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-02");
  MPI_CHECK(i != nullptr);
  MPI_CHECK(i->threshold_origin.find("configurable_heuristic") != std::string::npos);
  MPI_CHECK_MSG(i->threshold_origin.find("not a platform") != std::string::npos,
                "the 50 ms default must not be presented as an OS standard");
  // And the rule's declared threshold agrees.
  for (const auto& rule : rules::all_rules()) {
    if (rule->id() != "DET-02") continue;
    for (const auto& t : rule->thresholds()) {
      if (t.name != "long_task_ms") continue;
      MPI_CHECK(t.origin == rules::ThresholdOrigin::kConfigurableHeuristic);
      MPI_CHECK_EQ(t.to_json().find("is_platform_standard")->as_bool(), false);
    }
  }
}

MPI_TEST(det02_refuses_sampling_only_input, {"DET-02", "G01"}) {
  // A Hermes sampling profile has no task boundaries.
  auto t = load("traces/hermes-profile.json");
  const auto r = run(t);
  const auto* rec = record_for(r, "DET-02");
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK_MSG(contains(rec->skipped_reasons, "no task boundaries"),
                "task durations must not be invented from samples");
}

MPI_TEST(det02_ignores_spans_with_no_duration, {"D11"}) {
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  const auto* rec = record_for(r, "DET-02");
  MPI_CHECK(contains(rec->skipped_reasons, "no measured duration"));
}

MPI_TEST(threshold_override_changes_the_verdict, {"H04", "H09"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  symbols::SymbolService s;
  rules::EngineOptions o;
  o.mode = model::MeasurementMode::kDiagnostic;
  // Raise the long-task threshold above the fixture's worst task (180 ms).
  o.threshold_overrides.push_back({"DET-02.long_task_ms", 500.0});
  const auto r = rules::analyze(t, s, o);
  MPI_CHECK_MSG(first(r, "DET-02") == nullptr,
                "raising the threshold must suppress the finding");
  const auto* rec = record_for(r, "DET-02");
  MPI_CHECK_MSG(rec->outcome == model::RuleOutcome::kRanFoundNothing,
                "the rule still ran; it just found nothing");
}

MPI_TEST(det04_refuses_to_name_a_function_without_symbols, {"DET-04"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-04");
  MPI_CHECK(i != nullptr);
  MPI_CHECK_EQ(i->symbol_status, std::string("unavailable"));
  MPI_CHECK_MSG(i->title.find("unresolved frame") != std::string::npos,
                "without symbols the title must not present a function name");
  MPI_CHECK(contains(i->missing_evidence, "not a function name"));
}

MPI_TEST(det04_skips_below_minimum_sample_population, {"E14", "H03"}) {
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  const auto* rec = record_for(r, "DET-04");
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK(contains(rec->skipped_reasons, "would be noise"));
}

MPI_TEST(det04_skips_when_no_samples_and_says_it_is_not_idle, {"E13"}) {
  auto t = load("traces/positive-frames-js-cpu.mpi.json");
  t.cpu_samples.clear();
  const auto r = run(t);
  const auto* rec = record_for(r, "DET-04");
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK_MSG(contains(rec->skipped_reasons, "not idle time"),
                "a missing sample must not be read as idle");
}

MPI_TEST(det04_reports_self_share_as_disjoint_and_states_its_scope, {"E12", "F17"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-04");
  MPI_CHECK(i != nullptr);
  MPI_CHECK(!i->candidate_stacks.empty());
  MPI_CHECK_MSG(!i->candidate_stacks.front().inclusive,
                "a leaf self share is disjoint-comparable");
  const model::Metric* m = nullptr;
  for (const auto& x : i->metrics) {
    if (x.name == "cpu.sampled_self_share") m = &x;
  }
  MPI_CHECK(m != nullptr);
  MPI_CHECK(m->method == model::MetricMethod::kSampled);
  MPI_CHECK_MSG(contains(m->limitations, "not a share of a core"),
                "the normalization must be stated");
  MPI_CHECK(contains(m->limitations, "median sampling interval"));
}

MPI_TEST(det12_excludes_name_only_matches_from_attribution, {"section-9"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-12");
  MPI_CHECK(i != nullptr);
  MPI_CHECK(!r.attribution.empty());

  bool found_unclassified = false;
  bool found_devsupport = false;
  for (const auto& a : r.attribution) {
    for (const auto& s : a.slices) {
      if (s.label == "name_match_only_not_attributed") {
        found_unclassified = true;
        MPI_CHECK_MSG(s.category == model::AttributionCategory::kSharedOrUnknown,
                      "a name-only match must stay unclassified");
        MPI_CHECK(s.basis == model::AttributionBasis::kUnclassified);
      }
      if (s.label == "react_native_dev_support") {
        found_devsupport = true;
        MPI_CHECK(s.category == model::AttributionCategory::kDevelopmentTooling);
        MPI_CHECK(s.basis == model::AttributionBasis::kMatchingStack);
        MPI_CHECK_MSG(!s.rule_id.empty(), "attribution must cite its rule");
        MPI_CHECK_MSG(!s.rule_version.empty(), "attribution rules are versioned");
      }
    }
  }
  MPI_CHECK_MSG(found_unclassified,
                "the 'metrocache' name-only match must appear as unclassified");
  MPI_CHECK(found_devsupport);
}

MPI_TEST(det12_forbids_release_estimation_by_subtraction, {"F14"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  const auto* i = first(r, "DET-12");
  MPI_CHECK(i != nullptr);
  MPI_CHECK_MSG(contains(i->proposed_remediation, "do NOT subtract"),
                "the remediation must forbid subtracting the overhead");
  for (const auto& a : r.attribution) {
    MPI_CHECK_EQ(
        a.to_json().find("subtraction_to_estimate_release_permitted")->as_bool(),
        false);
    MPI_CHECK_MSG(!a.original_total.name.empty(),
                  "the original total must be preserved next to the slices");
  }
}

MPI_TEST(det12_preserves_original_total_alongside_slices, {"section-9"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  MPI_CHECK(!r.attribution.empty());
  const auto& a = r.attribution.front();
  MPI_CHECK(a.original_total.value.has_value());
  double slice_sum = 0.0;
  for (const auto& s : a.slices) slice_sum += s.value.value_or(0.0);
  MPI_CHECK_MSG(slice_sum <= *a.original_total.value,
                "attributed slices cannot exceed the original total");
}

MPI_TEST(diagnostic_session_never_reports_a_certified_benchmark, {"F14", "I16"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"),
                     model::MeasurementMode::kDiagnostic);
  MPI_CHECK(!r.eligibility.certified_benchmark());
  for (const auto& i : r.issues) {
    MPI_CHECK_MSG(!i.eligibility.certified_benchmark(),
                  "no issue from a diagnostic session may claim certification");
  }
}

MPI_TEST(ambiguous_ownership_is_surfaced_as_a_data_quality_note, {"B06", "B07"}) {
  const auto r = run(load("traces/ambiguous-ownership.mpi.json"));
  bool mentioned = false;
  for (const auto& n : r.data_quality_notes) {
    if (n.find("ambiguous") != std::string::npos &&
        n.find("excluded from app-scoped totals") != std::string::npos) {
      mentioned = true;
    }
  }
  MPI_CHECK_MSG(mentioned,
                "an unattributable process must be called out, not folded in");
}

MPI_TEST(partial_capture_is_surfaced_and_never_a_clean_pass, {"D19"}) {
  const auto r = run(load("traces/incomplete-evidence.mpi.json"));
  bool warned = false;
  for (const auto& n : r.data_quality_notes) {
    if (n.find("PARTIAL CAPTURE") != std::string::npos) warned = true;
  }
  MPI_CHECK(warned);
  MPI_CHECK(contains(r.data_quality_notes,
                     "absence of a finding is not evidence of absence"));
}

MPI_TEST(synthetic_input_is_labelled_at_the_top_level, {"H15", "section-0.6"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  MPI_CHECK(r.contains_synthetic_data);
  MPI_CHECK_EQ(r.to_json().find("synthetic")->as_bool(), true);
  MPI_CHECK(contains(r.data_quality_notes, "SYNTHETIC INPUT"));
  // And every evidence reference is individually marked.
  for (const auto& i : r.issues) {
    for (const auto& e : i.evidence) {
      MPI_CHECK_MSG(e.synthetic, "fixture evidence must be marked synthetic");
    }
  }
}

MPI_TEST(fingerprints_are_stable_across_reanalysis, {"section-15"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  const auto b = run(t);
  MPI_CHECK_EQ(a.issues.size(), b.issues.size());
  for (std::size_t i = 0; i < a.issues.size(); ++i) {
    MPI_CHECK_EQ(a.issues[i].fingerprint, b.issues[i].fingerprint);
    MPI_CHECK_EQ(a.issues[i].issue_id, b.issues[i].issue_id);
  }
}

MPI_TEST(issues_are_sorted_by_severity_then_stably, {"section-13"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  for (std::size_t i = 1; i < r.issues.size(); ++i) {
    const int prev = static_cast<int>(r.issues[i - 1].severity);
    const int cur = static_cast<int>(r.issues[i].severity);
    MPI_CHECK_MSG(prev >= cur, "issues must be ordered by descending severity");
    if (prev == cur) {
      MPI_CHECK(r.issues[i - 1].fingerprint <= r.issues[i].fingerprint);
    }
  }
}

MPI_TEST(every_evidence_reference_resolves_to_captured_data, {"H07"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto r = run(t);
  MPI_CHECK(!r.issues.empty());
  for (const auto& i : r.issues) {
    for (const auto& e : i.evidence) {
      if (e.kind == "frame") {
        const bool found = std::any_of(t.frames.begin(), t.frames.end(),
                                       [&](const model::FrameRecord& f) {
                                         return f.event_id == e.id;
                                       });
        MPI_CHECK_MSG(found, "dangling frame evidence id: " + e.id);
      } else if (e.kind == "js_task") {
        const bool found = std::any_of(t.js_tasks.begin(), t.js_tasks.end(),
                                       [&](const model::JsTask& j) {
                                         return j.event_id == e.id;
                                       });
        MPI_CHECK_MSG(found, "dangling js_task evidence id: " + e.id);
      }
      MPI_CHECK_MSG(!e.id.empty(), "an evidence reference must have an id");
    }
  }
}

MPI_TEST(issue_interval_lies_inside_the_capture_window, {"section-13"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto r = run(t);
  for (const auto& i : r.issues) {
    MPI_CHECK_MSG(i.start_ns >= t.window_start_ns,
                  "issue starts before the capture window");
    MPI_CHECK_MSG(i.end_ns <= t.window_end_ns + 1,
                  "issue ends after the capture window");
    MPI_CHECK_MSG(i.end_ns >= i.start_ns, "issue interval is inverted");
  }
}

MPI_TEST(every_candidate_cause_lists_missing_evidence, {"H17"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  for (const auto& i : r.issues) {
    if (i.cause_status == model::CauseStatus::kUnknown) continue;
    if (i.cause_status == model::CauseStatus::kExperimentallyVerified) continue;
    MPI_CHECK_MSG(!i.missing_evidence.empty(),
                  i.rule_id + ": a non-verified cause must list what is missing");
    MPI_CHECK_MSG(!i.alternative_explanations.empty(),
                  i.rule_id + ": a candidate cause must list alternatives");
  }
}

MPI_TEST(severity_is_independent_of_causal_confidence, {"H06"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  MPI_CHECK(!r.issues.empty());
  bool saw_high_with_unknown_cause = false;
  for (const auto& i : r.issues) {
    MPI_CHECK_MSG(!i.severity_rationale.empty(),
                  i.rule_id + " must explain what its severity means");
    // The rationale must say that severity is not a confidence in the cause.
    MPI_CHECK_MSG(contains(i.severity_rationale, "not a confidence") ||
                      contains(i.severity_rationale, "no claim about the cause") ||
                      contains(i.severity_rationale, "measurement-validity"),
                  i.rule_id + " severity rationale must separate impact from "
                              "causal confidence, got: " + i.severity_rationale);
    // A high-severity finding with an unknown cause is legitimate, and its
    // existence is what proves the two axes are independent.
    if (i.severity == model::Severity::kHigh &&
        i.cause_status == model::CauseStatus::kUnknown) {
      saw_high_with_unknown_cause = true;
    }
  }
  MPI_CHECK_MSG(saw_high_with_unknown_cause,
                "a high-impact symptom with an unestablished cause must be "
                "expressible; otherwise severity is acting as confidence");
}

MPI_TEST(no_issue_carries_an_uncalibrated_numeric_confidence, {"H16"}) {
  const auto r = run(load("traces/positive-frames-js-cpu.mpi.json"));
  for (const auto& i : r.issues) {
    // Confidence is prose with a stated basis, never a bare number.
    MPI_CHECK_MSG(!i.confidence_basis.empty(),
                  i.rule_id + " must state its confidence basis");
    const auto j = i.to_json();
    MPI_CHECK_MSG(j.find("confidence") == nullptr,
                  "no numeric confidence field may exist");
    MPI_CHECK_MSG(j.find("confidence_score") == nullptr,
                  "no numeric confidence field may exist");
  }
}

MPI_TEST(suppression_retains_reason_and_expiry, {"H10"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  symbols::SymbolService s;
  rules::EngineOptions o;
  rules::EngineOptions::Suppression sup;
  sup.rule_id = "DET-04";
  sup.reason = "known third-party rendering cost, tracked in PERF-1234";
  sup.expiry = "2026-12-31";
  sup.author = "tester";
  o.suppressions.push_back(sup);
  const auto r = rules::analyze(t, s, o);

  bool saw_suppressed = false;
  for (const auto& i : r.issues) {
    if (i.rule_id != "DET-04") continue;
    saw_suppressed = true;
    MPI_CHECK(i.suppressed);
    MPI_CHECK_EQ(i.suppression_reason, sup.reason);
    MPI_CHECK_EQ(i.suppression_expiry, sup.expiry);
    // The issue is retained, not deleted, so the suppression is auditable.
    const auto j = i.to_json();
    MPI_CHECK_EQ(j.find("suppression")->find("suppressed")->as_bool(), true);
    MPI_CHECK(!j.find("suppression")->find("reason")->is_null());
  }
  MPI_CHECK_MSG(saw_suppressed, "a suppressed issue must still be present");
}

MPI_TEST(cancellation_stops_analysis_and_marks_rules_skipped, {"D04", "J11"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  symbols::SymbolService s;
  CancellationSource src;
  src.cancel();
  rules::EngineOptions o;
  o.cancel = src.token();
  const auto r = rules::analyze(t, s, o);
  MPI_CHECK_MSG(r.issues.empty(), "a cancelled analysis must emit no issue");
  for (const auto& rec : r.rule_runs) {
    MPI_CHECK(rec.outcome == model::RuleOutcome::kSkipped);
  }
}

MPI_TEST(ruleset_and_engine_versions_are_recorded, {"H09"}) {
  const auto r = run(load("traces/negative-healthy.mpi.json"));
  MPI_CHECK(!r.ruleset_version.empty());
  MPI_CHECK(!r.engine_version.empty());
  MPI_CHECK(!r.analyzed_at.empty());
  for (const auto& rec : r.rule_runs) {
    MPI_CHECK_MSG(!rec.rule_version.empty(),
                  rec.rule_id + " must record its version");
  }
}
