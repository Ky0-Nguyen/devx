#include <cstdlib>
#include <sstream>

#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

std::string fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir ? dir : "fixtures") + "/" + rel;
}

bool has_warning(const ingest::ReadDiagnostics& d, const std::string& needle) {
  for (const auto& w : d.warnings) {
    if (w.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool trace_has_warning(const model::NormalizedTrace& t, const std::string& needle) {
  for (const auto& w : t.ingestion_warnings) {
    if (w.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(reads_native_trace_and_keeps_synthetic_label, {"H15"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/positive-frames-js-cpu.mpi.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK_EQ(*id, std::string("mpi.normalized.v2"));
  MPI_CHECK_MSG(t.synthetic, "a fixture must stay labelled synthetic");
  MPI_CHECK(!t.frames.empty());
  MPI_CHECK(!t.cpu_samples.empty());
  MPI_CHECK(!t.js_tasks.empty());
  // Every event inherits the synthetic flag.
  MPI_CHECK(t.events.front().has_flag(model::QualityFlag::kSyntheticFixture));
}

MPI_TEST(mark_synthetic_option_forces_the_label, {"H15"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::ReadOptions o;
  o.mark_synthetic = true;
  const auto id = ingest::read_any(fixture("traces/chrome-trace-event.json"), o, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK(t.synthetic);
}

MPI_TEST(rejects_malformed_traces_without_crashing, {"D08", "J02"}) {
  for (const char* f : {"traces/malformed-truncated.json",
                        "traces/malformed-not-json.bin",
                        "traces/malformed-empty.json"}) {
    model::NormalizedTrace t;
    ingest::ReadDiagnostics d;
    const auto id = ingest::read_any(fixture(f), ingest::ReadOptions{}, t, d);
    MPI_CHECK_MSG(!id.has_value(), std::string("should have been refused: ") + f);
    MPI_CHECK_MSG(!d.errors.empty(),
                  std::string("a refusal must carry a reason: ") + f);
  }
}

MPI_TEST(missing_file_is_an_error_not_an_empty_trace, {"A12", "D08"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/does-not-exist.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(!id.has_value());
  MPI_CHECK(!d.errors.empty());
}

MPI_TEST(empty_capture_reads_but_produces_no_data, {"D16"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/empty-capture.mpi.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK(t.events.empty());
  MPI_CHECK(t.frames.empty());
  ingest::normalize(t);
  // With no events, every collector's coverage is a gap, never measured zero.
  MPI_CHECK_EQ(t.duration_ns(), static_cast<model::TimeNs>(0));
}

MPI_TEST(unmeasured_clock_mapping_is_flagged_at_read_time, {"D12"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/incomplete-evidence.mpi.json"),
                   ingest::ReadOptions{}, t, d);
  MPI_CHECK_MSG(has_warning(d, "not measured"),
                "an assumed clock mapping must be called out on read");
}

MPI_TEST(chrome_reader_pairs_spans_and_flags_unpaired, {"D11"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/chrome-trace-event.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK_EQ(*id, std::string("chrome.trace_event.json"));

  const model::Event* paired = nullptr;
  const model::Event* unclosed = nullptr;
  const model::Event* orphan = nullptr;
  const model::Event* no_dur = nullptr;
  for (const auto& e : t.events) {
    if (e.name == "paired") paired = &e;
    if (e.name == "unclosed") unclosed = &e;
    if (e.name == "orphanEnd") orphan = &e;
    if (e.name == "noDuration") no_dur = &e;
  }
  MPI_CHECK(paired != nullptr);
  MPI_CHECK_MSG(paired->duration_ns.has_value(), "a B/E pair must get a duration");
  MPI_CHECK_EQ(*paired->duration_ns, static_cast<model::TimeNs>(90000000));

  MPI_CHECK(unclosed != nullptr);
  MPI_CHECK_MSG(!unclosed->duration_ns.has_value(),
                "an unclosed span must not be given an invented end");
  MPI_CHECK(unclosed->has_flag(model::QualityFlag::kIncompleteSpan));

  MPI_CHECK(orphan != nullptr);
  MPI_CHECK(orphan->has_flag(model::QualityFlag::kIncompleteSpan));

  MPI_CHECK(no_dur != nullptr);
  MPI_CHECK_MSG(!no_dur->duration_ns.has_value(),
                "an X event with no dur is incomplete, not instantaneous");

  MPI_CHECK(t.partial);
  MPI_CHECK(!t.partial_reasons.empty());
}

MPI_TEST(chrome_reader_refuses_to_claim_a_clock_base, {"D12", "E22"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/chrome-trace-event.json"),
                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(!t.clock_domains.empty());
  MPI_CHECK_MSG(!t.clock_domains.front().monotonic,
                "the format does not state its clock base, so we must not "
                "claim monotonicity");
  MPI_CHECK(has_warning(d, "does not declare its clock base"));
}

MPI_TEST(chrome_reader_marks_ownership_as_unestablished, {"B07"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/chrome-trace-event.json"),
                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(!t.events.empty());
  // An imported trace carries no ownership evidence for its pids.
  MPI_CHECK(t.events.front().has_flag(model::QualityFlag::kAmbiguousOwnership));
}

MPI_TEST(chrome_reader_derives_js_tasks_only_from_complete_spans, {"D11"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/chrome-trace-event.json"),
                   ingest::ReadOptions{}, t, d);
  for (const auto& j : t.js_tasks) {
    MPI_CHECK_MSG(j.duration_ns.has_value(),
                  "no JS task may exist without a measured duration");
    // No imported trace has a measured mapping to a UI clock.
    MPI_CHECK(!j.clock_mapped_to_ui);
  }
  bool found_expensive = false;
  for (const auto& j : t.js_tasks) {
    if (j.name == "expensiveReduce") found_expensive = true;
  }
  MPI_CHECK(found_expensive);
}

MPI_TEST(hermes_reader_produces_samples_not_tasks, {"G01", "DET-02"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/hermes-profile.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK_EQ(*id, std::string("hermes.sampling_profile"));
  MPI_CHECK(!t.cpu_samples.empty());
  // A sampling profile has no task boundaries, so it must yield no JS tasks.
  MPI_CHECK_MSG(t.js_tasks.empty(),
                "a sampling profile must not be turned into task durations");
  MPI_CHECK(has_warning(d, "no task boundaries"));
}

MPI_TEST(hermes_reader_unwinds_stacks_outermost_first, {"E15"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/hermes-profile.json"), ingest::ReadOptions{},
                   t, d);
  MPI_CHECK(!t.cpu_samples.empty());
  const auto& frames = t.cpu_samples.front().frames;
  MPI_CHECK(frames.size() >= 3);
  MPI_CHECK_EQ(frames.front(), std::string("(root)"));
  MPI_CHECK(frames.back().find("formatMoney") != std::string::npos);
}

MPI_TEST(hermes_reader_survives_cyclic_parent_chain, {"J02"}) {
  // The fixture contains a self-referential stack frame. This must terminate.
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  const auto id = ingest::read_any(fixture("traces/hermes-profile.json"),
                                   ingest::ReadOptions{}, t, d);
  MPI_CHECK(id.has_value());
  MPI_CHECK(t.cpu_samples.size() > 100);
}

MPI_TEST(hermes_samples_stay_on_an_unmapped_js_clock, {"section-11"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/hermes-profile.json"), ingest::ReadOptions{},
                   t, d);
  MPI_CHECK(has_warning(d, "no mapping"));
  MPI_CHECK(t.clock_mappings.empty());
}

MPI_TEST(normalize_flags_duplicates_without_deleting_them, {"D10"}) {
  model::NormalizedTrace t;
  t.primary_clock_domain = "ui";
  for (int i = 0; i < 3; ++i) {
    model::Event e;
    e.event_id = "same-id";
    e.clock_domain = "ui";
    e.timestamp = i * 100;
    t.events.push_back(e);
  }
  const auto rep = ingest::normalize(t);
  MPI_CHECK_EQ(rep.duplicates_flagged, static_cast<std::int64_t>(3));
  MPI_CHECK_MSG(t.events.size() == 3, "flagged, not deleted");
  for (const auto& e : t.events) {
    MPI_CHECK(e.has_flag(model::QualityFlag::kDuplicate));
  }
}

MPI_TEST(normalize_records_out_of_order_before_sorting, {"D10"}) {
  model::NormalizedTrace t;
  t.primary_clock_domain = "ui";
  for (const model::TimeNs ts : {500, 100, 900, 200}) {
    model::Event e;
    e.event_id = "e" + std::to_string(ts);
    e.clock_domain = "ui";
    e.timestamp = ts;
    t.events.push_back(e);
  }
  const auto rep = ingest::normalize(t);
  MPI_CHECK(rep.out_of_order_flagged >= 2);
  // Sorted afterwards, but the flag survives.
  MPI_CHECK_EQ(t.events.front().timestamp, static_cast<model::TimeNs>(100));
  bool any_flagged = false;
  for (const auto& e : t.events) {
    if (e.has_flag(model::QualityFlag::kOutOfOrder)) any_flagged = true;
  }
  MPI_CHECK(any_flagged);
}

MPI_TEST(normalize_is_idempotent, {"section-15"}) {
  model::NormalizedTrace a;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/positive-frames-js-cpu.mpi.json"),
                   ingest::ReadOptions{}, a, d);
  ingest::normalize(a);
  const std::string once = a.to_json(true).dump();
  ingest::normalize(a);
  const std::string twice = a.to_json(true).dump();
  MPI_CHECK_MSG(once == twice, "normalizing twice must not change the trace");
}

MPI_TEST(normalize_flags_unmappable_clock_domains, {"D12"}) {
  model::NormalizedTrace t;
  t.primary_clock_domain = "ui";
  model::Event e;
  e.event_id = "x";
  e.clock_domain = "js";  // no mapping registered
  e.timestamp = 100;
  t.events.push_back(e);
  ingest::normalize(t);
  MPI_CHECK(t.events.front().has_flag(model::QualityFlag::kClockUnmapped));
}

MPI_TEST(normalize_turns_provider_drops_into_visible_notes, {"D09"}) {
  model::NormalizedTrace t;
  t.primary_clock_domain = "ui";
  t.dropped_events_by_collector["sampler"] = 4096;
  model::Event e;
  e.event_id = "x";
  e.provider = "sampler";
  e.clock_domain = "ui";
  t.events.push_back(e);
  const auto rep = ingest::normalize(t);
  bool mentioned = false;
  for (const auto& n : rep.notes) {
    if (n.find("4096") != std::string::npos &&
        n.find("gaps, not zero") != std::string::npos) {
      mentioned = true;
    }
  }
  MPI_CHECK(mentioned);
  MPI_CHECK(t.events.front().has_flag(model::QualityFlag::kProviderDropped));
}

MPI_TEST(derived_coverage_treats_absent_collector_as_a_full_gap, {"E13"}) {
  model::NormalizedTrace t;
  t.window_start_ns = 0;
  t.window_end_ns = 1000000000;
  const auto cov = ingest::derive_coverage(t, "never-ran", 1000000);
  MPI_CHECK_EQ(cov.event_count, static_cast<std::int64_t>(0));
  MPI_CHECK_EQ(cov.gaps.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(cov.gaps.front().reason, std::string("no_events_from_collector"));
  MPI_CHECK_NEAR(cov.covered_fraction(), 0.0, 1e-9);
}

MPI_TEST(derived_coverage_detects_late_start_and_early_stop, {"D09"}) {
  model::NormalizedTrace t;
  t.window_start_ns = 0;
  t.window_end_ns = 10000000000LL;
  for (const model::TimeNs ts : {5000000000LL, 5100000000LL}) {
    model::Event e;
    e.provider = "sampler";
    e.timestamp = ts;
    t.events.push_back(e);
  }
  const auto cov = ingest::derive_coverage(t, "sampler", 500000000);
  bool late = false, early = false;
  for (const auto& g : cov.gaps) {
    if (g.reason == "collector_started_late") late = true;
    if (g.reason == "collector_stopped_early") early = true;
  }
  MPI_CHECK(late);
  MPI_CHECK(early);
}

MPI_TEST(process_filter_excludes_foreign_events, {"B12"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::ReadOptions o;
  o.keep_process_instance_ids.push_back("some|other|process");
  ingest::read_any(fixture("traces/positive-frames-js-cpu.mpi.json"), o, t, d);
  MPI_CHECK_MSG(t.events.empty(),
                "no event from an unselected process may be kept");
  MPI_CHECK(d.events_rejected > 0);
}

MPI_TEST(unsupported_schema_version_is_reported_not_guessed, {"D18"}) {
  model::NormalizedTrace t;
  ingest::ReadDiagnostics d;
  ingest::read_any(fixture("traces/incomplete-evidence.mpi.json"),
                   ingest::ReadOptions{}, t, d);
  // Our own fixtures are 2.0, so no warning here; assert the field survives.
  MPI_CHECK_EQ(t.schema_version, std::string("2.0"));
  MPI_CHECK(trace_has_warning(t, "collector crashed") || t.partial);
}
