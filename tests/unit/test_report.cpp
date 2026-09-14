#include <cstdlib>
#include <sstream>

#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
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

model::AnalysisResult run(const model::NormalizedTrace& t) {
  symbols::SymbolService s;
  rules::EngineOptions o;
  o.mode = model::MeasurementMode::kDiagnostic;
  return rules::analyze(t, s, o);
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

MPI_TEST(markdown_and_json_agree_on_issue_count, {"H12"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  report::ReportOptions o;
  const auto md = report::to_markdown(t, a, o);
  const auto js = report::to_json(t, a, o);

  MPI_CHECK(has(md, "## Issues (" + std::to_string(a.issues.size()) + ")"));
  json::ParseError err;
  auto parsed = json::parse(js, &err);
  MPI_CHECK_MSG(parsed.has_value(), "JSON report must parse: " + err.message);
  const json::Value* issues = parsed->find("analysis")->find("issues");
  MPI_CHECK_EQ(issues->size(), a.issues.size());
  // Every issue in the JSON also appears by fingerprint in the Markdown.
  for (const auto& i : issues->items()) {
    MPI_CHECK(has(md, i.find("fingerprint")->as_string()));
  }
}

MPI_TEST(json_report_is_valid_and_carries_the_schema_version, {"H12"}) {
  const auto t = load("traces/incomplete-evidence.mpi.json");
  const auto a = run(t);
  const auto js = report::to_json(t, a, report::ReportOptions{});
  json::ParseError err;
  auto parsed = json::parse(js, &err);
  MPI_CHECK(parsed.has_value());
  MPI_CHECK_EQ(parsed->find("schema_version")->as_string(), std::string("2.0"));
  MPI_CHECK(parsed->find("trace") != nullptr);
  MPI_CHECK(parsed->find("analysis") != nullptr);
}

MPI_TEST(synthetic_data_is_announced_in_both_formats, {"H15", "section-0.6"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "SYNTHETIC DATA"));
  MPI_CHECK_MSG(md.find("SYNTHETIC DATA") < 400,
                "the synthetic warning must appear near the top");
  const auto js = report::to_json(t, a, report::ReportOptions{});
  auto parsed = json::parse(js, nullptr);
  MPI_CHECK_EQ(parsed->find("analysis")->find("synthetic")->as_bool(), true);
  MPI_CHECK_EQ(parsed->find("trace")->find("synthetic")->as_bool(), true);
}

MPI_TEST(skipped_detectors_and_their_reasons_appear_in_the_report, {"H05", "H11"}) {
  const auto t = load("traces/incomplete-evidence.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "## Detector execution"));
  MPI_CHECK(has(md, "could not run"));
  MPI_CHECK(has(md, "`DET-04`"));
  MPI_CHECK(has(md, "would be noise"));
  MPI_CHECK_MSG(has(md, "found **nothing because it did not run**"),
                "the report must state that a skip is not an all-clear");
}

MPI_TEST(no_findings_is_distinguished_from_no_analysis, {"H11"}) {
  const auto t = load("traces/negative-healthy.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "## Issues (0)"));
  MPI_CHECK(has(md, "No detector that ran produced a finding"));
  MPI_CHECK_MSG(has(md, "ran_found_nothing"),
                "the report must show that detectors actually ran");
}

MPI_TEST(source_paths_are_excluded_from_export_by_default, {"section-14", "J01"}) {
  auto t = load("traces/positive-frames-js-cpu.mpi.json");
  // Give one issue a source location so there is something to redact.
  auto a = run(t);
  MPI_CHECK(!a.issues.empty());
  for (auto& i : a.issues) {
    if (i.candidate_stacks.empty()) continue;
    model::SourceLocation loc;
    loc.file = "/Users/someone/private/repo/src/Secret.tsx";
    loc.line = 42;
    loc.symbol_status = "exact_build_match";
    i.candidate_stacks.front().locations.clear();
    i.candidate_stacks.front().locations.push_back(loc);
    break;
  }

  report::ReportOptions excluded;
  excluded.include_source_paths = false;
  const auto js_excluded = report::to_json(t, a, excluded);
  MPI_CHECK_MSG(!has(js_excluded, "private/repo/src/Secret.tsx"),
                "a source path must not leave the machine by default");
  MPI_CHECK(has(js_excluded, "source path omitted from export"));

  report::ReportOptions included;
  included.include_source_paths = true;
  const auto js_included = report::to_json(t, a, included);
  MPI_CHECK_MSG(has(js_included, "private/repo/src/Secret.tsx"),
                "an explicit opt-in must include the path");
}

MPI_TEST(markdown_escapes_table_breaking_and_html_characters, {"section-15"}) {
  MPI_CHECK_EQ(report::escape_markdown("a|b"), std::string("a\\|b"));
  MPI_CHECK_EQ(report::escape_markdown("`code`"), std::string("\\`code\\`"));
  MPI_CHECK_EQ(report::escape_markdown("<script>"), std::string("&lt;script&gt;"));
  MPI_CHECK_EQ(report::escape_markdown("line\nbreak"), std::string("line break"));
  MPI_CHECK_EQ(report::escape_markdown("a\r\nb"), std::string("a b"));
}

MPI_TEST(hostile_identifiers_cannot_break_the_markdown_table, {"section-15", "J01"}) {
  auto t = load("traces/negative-healthy.mpi.json");
  // A trace field is untrusted input; it must not be able to inject markup.
  t.target.app.app_identifier = "com.evil|app<script>alert(1)</script>";
  t.device.display_name = "dev|ice`rm -rf`";
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK_MSG(!has(md, "<script>"), "raw HTML must not survive into the export");
  MPI_CHECK(has(md, "&lt;script&gt;"));
  MPI_CHECK(has(md, "com.evil\\|app"));
  MPI_CHECK(has(md, "dev\\|ice"));
}

MPI_TEST(unknown_values_are_shown_as_unknown_not_omitted, {"C18", "section-13"}) {
  const auto t = load("traces/incomplete-evidence.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  // The incomplete fixture has no optimization fact at all.
  MPI_CHECK(has(md, "## Benchmark eligibility") || has(md, "Benchmark eligibility"));
  MPI_CHECK_MSG(has(md, "cannot certify release performance"),
                "a session that cannot certify must say so");
  MPI_CHECK(has(md, "Every reason, not just the first"));
}

MPI_TEST(coverage_gaps_are_reported_with_their_semantics, {"D09", "E13"}) {
  const auto t = load("traces/incomplete-evidence.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "## Collector coverage"));
  MPI_CHECK_MSG(has(md, "is a **gap**, not measured zero activity"),
                "the gap-vs-zero distinction must be stated in the report");
}

MPI_TEST(partial_capture_is_announced, {"D19"}) {
  const auto t = load("traces/incomplete-evidence.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "PARTIAL CAPTURE"));
  MPI_CHECK(has(md, "collector crashed"));
}

MPI_TEST(report_explains_how_to_read_detection_versus_cause, {"section-10.1"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "## How to read this report"));
  MPI_CHECK(has(md, "Cause status is separate from detection status"));
  MPI_CHECK(has(md, "Severity orders impact. It is not a confidence"));
  MPI_CHECK(has(md, "candidate` cause, never a proven one"));
  MPI_CHECK(has(md, "never reported as zero"));
}

MPI_TEST(suppressed_issues_can_be_omitted_or_kept_with_their_reason, {"H10"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  auto a = run(t);
  MPI_CHECK(!a.issues.empty());
  a.issues.front().suppressed = true;
  a.issues.front().suppression_reason = "tracked in PERF-9999";
  a.issues.front().suppression_expiry = "2027-01-01";

  report::ReportOptions keep;
  keep.include_suppressed = true;
  const auto md_keep = report::to_markdown(t, a, keep);
  MPI_CHECK(has(md_keep, "**Suppressed.**"));
  MPI_CHECK(has(md_keep, "tracked in PERF-9999"));
  MPI_CHECK(has(md_keep, "expires 2027-01-01"));

  report::ReportOptions drop;
  drop.include_suppressed = false;
  const auto js_drop = report::to_json(t, a, drop);
  auto parsed = json::parse(js_drop, nullptr);
  MPI_CHECK_EQ(parsed->find("analysis")->find("issues")->size(),
               a.issues.size() - 1);
  MPI_CHECK_EQ(
      parsed->find("analysis")->find("suppressed_issues_omitted")->as_bool(),
      true);
}

MPI_TEST(attribution_section_forbids_subtraction_in_prose, {"F14"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  MPI_CHECK(!a.attribution.empty());
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "## Resource attribution"));
  MPI_CHECK(has(md, "**Original total:**"));
  MPI_CHECK_MSG(has(md, "must not be subtracted"),
                "the report must forbid subtracting overhead in prose too");
}

MPI_TEST(inclusive_stacks_are_labelled_in_the_markdown, {"F17"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  auto a = run(t);
  for (auto& i : a.issues) {
    if (i.candidate_stacks.empty()) continue;
    i.candidate_stacks.front().inclusive = true;
    break;
  }
  const auto md = report::to_markdown(t, a, report::ReportOptions{});
  MPI_CHECK(has(md, "not** summable as a disjoint cost"));
}

MPI_TEST(comparison_report_renders_in_both_formats, {"H12"}) {
  session::RunSet b;
  b.label = "baseline";
  b.conditions.platform = "android";
  b.conditions.scenario_id = "checkout";
  b.mode = model::MeasurementMode::kBenchmark;
  session::RunSet c = b;
  c.label = "candidate";
  c.conditions.platform = "ios";  // deliberately cross-platform
  const auto cmp = session::compare(std::move(b), std::move(c),
                                    session::ComparisonThresholds{});
  const auto md = report::comparison_to_markdown(cmp);
  MPI_CHECK(has(md, "# Comparison report"));
  MPI_CHECK(has(md, "Overall verdict: `inconclusive`"));
  MPI_CHECK(has(md, "cross-platform pair"));
  MPI_CHECK(has(md, "cannot be used as a gate"));

  const auto js = report::comparison_to_json(cmp);
  json::ParseError err;
  auto parsed = json::parse(js, &err);
  MPI_CHECK_MSG(parsed.has_value(), err.message);
  MPI_CHECK_EQ(parsed->find("report_kind")->as_string(), std::string("comparison"));
}

MPI_TEST(raw_events_are_excluded_from_the_export_by_default, {"section-14"}) {
  const auto t = load("traces/positive-frames-js-cpu.mpi.json");
  const auto a = run(t);
  report::ReportOptions off;
  off.include_raw_events = false;
  const auto small = report::to_json(t, a, off);
  report::ReportOptions on;
  on.include_raw_events = true;
  const auto large = report::to_json(t, a, on);
  MPI_CHECK_MSG(large.size() > small.size() * 2,
                "including raw events should materially grow the export");
  auto parsed = json::parse(small, nullptr);
  MPI_CHECK(parsed->find("trace")->find("events") == nullptr);
  // But the counts are always present, so nothing looks empty.
  MPI_CHECK(parsed->find("trace")->find("counts") != nullptr);
}
