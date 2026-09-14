#include <algorithm>
#include <sstream>

#include "core/report/report.hpp"
#include "core/util/time.hpp"

namespace mpi::report {
namespace {

std::string pct(double fraction) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(1);
  os << (fraction * 100.0) << "%";
  return os.str();
}

std::string num(const std::optional<double>& v, const std::string& unit) {
  if (!v.has_value()) return "*not measured*";
  std::ostringstream os;
  if (unit == "ns") {
    return time_util::format_duration_ns(static_cast<std::int64_t>(*v));
  }
  if (unit == "fraction") return pct(*v);
  os.setf(std::ios::fixed);
  os.precision(*v == static_cast<double>(static_cast<long long>(*v)) ? 0 : 3);
  os << *v;
  if (!unit.empty() && unit != "count") os << " " << unit;
  return os.str();
}

void bullets(std::ostringstream& os, const char* label,
             const std::vector<std::string>& items) {
  if (items.empty()) return;
  os << "**" << label << "**\n\n";
  for (const auto& i : items) os << "- " << escape_markdown(i) << "\n";
  os << "\n";
}

}  // namespace

std::string escape_markdown(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    // Pipes break tables; backticks and angle brackets can inject formatting
    // or HTML into the export (spec section 15).
    switch (c) {
      case '|': out += "\\|"; break;
      case '`': out += "\\`"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\n': out += " "; break;
      case '\r': break;
      default: out.push_back(c);
    }
  }
  return out;
}

// One issue, rendered the same way wherever it appears. The comparison
// report shows DET-08 findings with exactly the fields a session report
// shows, so a reader does not have to learn two layouts -- and a field
// added to the contract cannot appear in one report and not the other.
void write_issue(std::ostringstream& os, const model::Issue& i,
                 bool include_source_paths) {
  os << "### " << escape_markdown(i.title) << "\n\n";
  if (i.suppressed) {
    os << "> **Suppressed.** " << escape_markdown(i.suppression_reason);
    if (!i.suppression_expiry.empty()) {
      os << " (expires " << escape_markdown(i.suppression_expiry) << ")";
    }
    if (!i.suppression_author.empty()) {
      os << " -- " << escape_markdown(i.suppression_author);
    }
    os << "\n\n";
  }
  os << "| | |\n|---|---|\n";
  os << "| Detector | `" << escape_markdown(i.rule_id) << "` v"
     << escape_markdown(i.rule_version) << " |\n";
  os << "| Severity | **" << model::to_string(i.severity) << "** |\n";
  os << "| Detection status | `" << model::to_string(i.detection_status)
     << "` |\n";
  os << "| Cause status | `" << model::to_string(i.cause_status) << "` |\n";
  os << "| Interval | " << time_util::format_duration_ns(i.start_ns) << " -> "
     << time_util::format_duration_ns(i.end_ns) << " ("
     << time_util::format_duration_ns(i.end_ns - i.start_ns) << ") |\n";
  os << "| Process | `" << escape_markdown(i.process_instance_id) << "` |\n";
  if (!i.thread_instance_id.empty()) {
    os << "| Thread | `" << escape_markdown(i.thread_instance_id) << "` |\n";
  }
  os << "| Screen | "
     << (i.screen.empty() ? "*not observed*" : escape_markdown(i.screen))
     << " |\n";
  os << "| Occurrences | " << i.occurrence_count << " |\n";
  os << "| Symbol status | `" << escape_markdown(i.symbol_status) << "` |\n";
  os << "| Fingerprint | `" << escape_markdown(i.fingerprint) << "` |\n";
  os << "\n";

  os << "Severity means: " << escape_markdown(i.severity_rationale) << "\n\n";
  if (!i.confidence_basis.empty()) {
    os << "**What the evidence supports.** " << escape_markdown(i.confidence_basis)
       << "\n\n";
  }

  if (!i.metrics.empty()) {
    os << "**Metrics**\n\n";
    os << "| Metric | Value | Method | Aggregation | Limitations |\n"
          "|---|---|---|---|---|\n";
    for (const auto& m : i.metrics) {
      os << "| `" << escape_markdown(m.name) << "` | " << num(m.value, m.unit)
         << " | " << model::to_string(m.method) << " | "
         << escape_markdown(m.aggregation) << " | ";
      if (m.limitations.empty()) {
        os << "--";
      } else {
        bool first = true;
        for (const auto& l : m.limitations) {
          if (!first) os << "<br>";
          first = false;
          os << escape_markdown(l);
        }
      }
      os << " |\n";
    }
    os << "\n";
  }

  if (!i.threshold_expression.empty()) {
    os << "**Threshold.** `" << escape_markdown(i.threshold_expression)
       << "` -- origin: " << escape_markdown(i.threshold_origin) << "\n\n";
  }

  bullets(os, "Missing evidence", i.missing_evidence);
  bullets(os, "Alternative explanations", i.alternative_explanations);
  bullets(os, "Suggested verification", i.suggested_verification);
  bullets(os, "Proposed remediation", i.proposed_remediation);

  if (!i.candidate_stacks.empty()) {
    os << "**Candidate stacks**\n\n";
    for (const auto& s : i.candidate_stacks) {
      os << "- share ";
      os << (s.sample_share.has_value() ? pct(*s.sample_share)
                                        : std::string("*unknown*"));
      os << (s.inclusive ? " (inclusive -- **not** summable as a disjoint cost)"
                         : " (self)")
         << "\n";
      for (std::size_t fi = 0; fi < s.frames.size() && fi < 12; ++fi) {
        os << "  " << std::string(2, ' ') << "- `"
           << escape_markdown(s.frames[fi]) << "`";
        if (fi < s.locations.size()) {
          const auto& l = s.locations[fi];
          if (include_source_paths && !l.file.empty()) {
            os << " -> " << escape_markdown(l.file);
            if (l.line.has_value()) os << ":" << *l.line;
          }
          os << " [" << escape_markdown(l.symbol_status) << "]";
          if (!l.safe_to_open() && !l.note.empty()) {
            os << " -- " << escape_markdown(l.note);
          }
        }
        os << "\n";
      }
    }
    os << "\n";
  }

  if (!i.evidence.empty()) {
    os << "**Evidence** (" << i.evidence.size() << " reference(s))\n\n";
    os << "| Kind | Id | Interval | Note |\n|---|---|---|---|\n";
    for (const auto& e : i.evidence) {
      os << "| " << escape_markdown(e.kind) << " | `"
         << escape_markdown(e.id) << "` | ";
      if (e.start_ns.has_value()) {
        os << time_util::format_duration_ns(*e.start_ns);
        if (e.end_ns.has_value()) {
          os << " -> " << time_util::format_duration_ns(*e.end_ns);
        }
      } else {
        os << "--";
      }
      os << " | " << escape_markdown(e.note) << (e.synthetic ? " *(synthetic)*" : "")
         << " |\n";
    }
    os << "\n";
  }
  os << "---\n\n";
}

std::string to_markdown(const model::NormalizedTrace& trace,
                        const model::AnalysisResult& analysis,
                        const ReportOptions& opts) {
  std::ostringstream os;

  os << "# Mobile Performance Inspector -- session report\n\n";

  if (trace.synthetic) {
    os << "> **SYNTHETIC DATA.** This report was produced from a labelled "
          "fixture, not a real device capture. Nothing here describes the "
          "behavior of a real application.";
    if (!trace.synthetic_note.empty()) {
      os << " " << escape_markdown(trace.synthetic_note);
    }
    os << "\n\n";
  }

  // Measurement context before conclusions, always.
  os << "## Measurement context\n\n";
  os << "| Field | Value |\n|---|---|\n";
  os << "| Session | `" << escape_markdown(trace.session_id) << "` |\n";
  os << "| Analyzed at | " << escape_markdown(analysis.analyzed_at) << " |\n";
  os << "| Engine / ruleset | " << escape_markdown(analysis.engine_version)
     << " / " << escape_markdown(analysis.ruleset_version) << " |\n";
  os << "| Platform | " << model::to_string(trace.device.platform) << " |\n";
  os << "| Device | " << escape_markdown(trace.device.display_name.empty()
                                             ? trace.device.device_id
                                             : trace.device.display_name)
     << " (" << model::to_string(trace.device.form) << ", "
     << escape_markdown(trace.device.model) << ", OS "
     << escape_markdown(trace.device.os_version) << ") |\n";
  os << "| Target | `" << escape_markdown(trace.target.app.app_identifier)
     << "` (" << model::to_string(trace.target.app.identifier_kind) << ") |\n";
  os << "| Runtime state at capture | "
     << model::to_string(trace.target.runtime_state_at_capture) << " |\n";
  os << "| Discovery scope | " << model::to_string(trace.target.discovery_scope)
     << " |\n";
  os << "| Measurement mode | " << model::to_string(analysis.mode) << " |\n";
  os << "| Capture window | "
     << time_util::format_duration_ns(trace.duration_ns()) << " |\n";
  os << "| Primary clock domain | `"
     << escape_markdown(trace.primary_clock_domain) << "` |\n";
  os << "| Partial capture | " << (trace.partial ? "**yes**" : "no") << " |\n";
  os << "\n";

  // Eligibility, stated as its own section because it governs what the numbers
  // may be used for.
  os << "### Benchmark eligibility: `"
     << model::to_string(analysis.eligibility.status) << "`\n\n";
  if (analysis.eligibility.certified_benchmark()) {
    os << "This session is eligible for production-like comparison. "
          "Eligibility does not mean zero profiler overhead.\n\n";
  } else {
    os << "This session **cannot certify release performance**";
    if (analysis.mode == model::MeasurementMode::kDiagnostic) {
      os << " -- it is a diagnostic session, which is for investigating "
            "behavior, not for certifying it";
    }
    os << ".\n\n";
  }
  if (!analysis.eligibility.reasons.empty()) {
    os << "Every reason, not just the first:\n\n";
    for (const auto& r : analysis.eligibility.reasons) {
      os << "- `" << escape_markdown(r) << "`\n";
    }
    os << "\n";
  }
  if (analysis.eligibility.user_override) {
    os << "> A user override is recorded on this session. An override permits "
          "exploratory comparison; it does not make an invalid measurement "
          "valid.";
    if (!analysis.eligibility.user_override_note.empty()) {
      os << " Note: " << escape_markdown(analysis.eligibility.user_override_note);
    }
    os << "\n\n";
  }

  // Build facts, with unknown shown as unknown.
  if (!trace.build.facts.empty()) {
    os << "## Build and runtime facts\n\n";
    os << "| Fact | Value | Source |\n|---|---|---|\n";
    for (const auto& f : trace.build.facts) {
      os << "| `" << escape_markdown(f.key) << "` | ";
      if (f.boolean_value != model::Tri::kUnknown) {
        os << "`" << model::to_string(f.boolean_value) << "`";
      } else if (!f.value.empty()) {
        os << escape_markdown(f.value);
      } else {
        os << "*unknown*";
      }
      os << " | " << model::to_string(f.source) << " |\n";
    }
    os << "\n";
  }
  if (!trace.build.conflicts.empty()) {
    os << "### Conflicting facts\n\n"
          "These disagree and were not silently resolved:\n\n";
    for (const auto& c : trace.build.conflicts) {
      os << "- `" << escape_markdown(c.key) << "`: "
         << model::to_string(c.a.source) << " says \""
         << escape_markdown(c.a.value) << "\", "
         << model::to_string(c.b.source) << " says \""
         << escape_markdown(c.b.value) << "\"\n";
    }
    os << "\n";
  }

  // Coverage: what was actually collected, so absence of a finding can be read
  // correctly.
  if (!analysis.coverage.empty()) {
    os << "## Collector coverage\n\n";
    os << "| Collector | Events | Coverage | Gaps |\n|---|---|---|---|\n";
    for (const auto& c : analysis.coverage) {
      os << "| `" << escape_markdown(c.collector) << "` | " << c.event_count
         << " | " << pct(c.covered_fraction()) << " | " << c.gaps.size() << " |\n";
    }
    os << "\nAn uncovered interval is a **gap**, not measured zero activity.\n\n";
  }

  if (!analysis.data_quality_notes.empty()) {
    os << "## Data quality\n\n";
    for (const auto& n : analysis.data_quality_notes) {
      os << "- " << escape_markdown(n) << "\n";
    }
    os << "\n";
  }

  // Rule execution table: this is what makes "no findings" different from
  // "no analysis".
  os << "## Detector execution\n\n";
  std::size_t ran = 0, skipped = 0, found = 0;
  for (const auto& r : analysis.rule_runs) {
    if (r.outcome == model::RuleOutcome::kSkipped) ++skipped;
    else ++ran;
    if (r.outcome == model::RuleOutcome::kRanFoundIssues) ++found;
  }
  os << ran << " detector(s) ran, " << found << " found something, " << skipped
     << " could not run.\n\n";
  os << "| Detector | Outcome | Issues | Why it could not run |\n|---|---|---|---|\n";
  for (const auto& r : analysis.rule_runs) {
    os << "| `" << escape_markdown(r.rule_id) << "` v"
       << escape_markdown(r.rule_version) << " | `"
       << model::to_string(r.outcome) << "` | " << r.issues_emitted << " | ";
    if (r.skipped_reasons.empty()) {
      os << "--";
    } else {
      bool first = true;
      for (const auto& s : r.skipped_reasons) {
        if (!first) os << "<br>";
        first = false;
        os << escape_markdown(s);
      }
    }
    os << " |\n";
  }
  os << "\n> A `skipped` detector found **nothing because it did not run**. "
        "That is not the same statement as \"no issue exists\".\n\n";

  // Issues.
  std::vector<const model::Issue*> shown;
  for (const auto& i : analysis.issues) {
    if (!opts.include_suppressed && i.suppressed) continue;
    shown.push_back(&i);
  }

  os << "## Issues (" << shown.size() << ")\n\n";
  if (shown.empty()) {
    os << "No detector that ran produced a finding. See the detector table "
          "above for which detectors could not run and why.\n\n";
  }

  for (const auto* ip : shown) {
    write_issue(os, *ip, opts.include_source_paths);
  }

  if (!analysis.attribution.empty()) {
    os << "## Resource attribution\n\n";
    for (const auto& a : analysis.attribution) {
      os << "**Original total:** `" << escape_markdown(a.original_total.name)
         << "` = " << num(a.original_total.value, a.original_total.unit) << " ("
         << model::to_string(a.original_total.method) << ")\n\n";
      os << "| Category | Label | Value | Locus | Basis | Rule |\n"
            "|---|---|---|---|---|---|\n";
      for (const auto& s : a.slices) {
        os << "| " << model::to_string(s.category) << " | "
           << escape_markdown(s.label) << " | " << num(s.value, s.unit) << " | "
           << escape_markdown(s.locus) << " | " << model::to_string(s.basis)
           << " | `" << escape_markdown(s.rule_id) << "` v"
           << escape_markdown(s.rule_version) << " |\n";
      }
      os << "\n";
      for (const auto& l : a.limitations) os << "- " << escape_markdown(l) << "\n";
      os << "\n> These slices **must not be subtracted** from the original "
            "total to estimate release performance.\n\n";
    }
  }

  os << "## How to read this report\n\n"
        "- `observed` means measured. `suspected` means the evidence is "
        "indirect or came from a proxy source. `inconclusive` means the "
        "detector could not decide.\n"
        "- Cause status is separate from detection status: a measured symptom "
        "with `cause_status: unknown` is the normal, honest result.\n"
        "- Severity orders impact. It is not a confidence in any cause.\n"
        "- A temporal correlation is a `candidate` cause, never a proven one.\n"
        "- Missing data is reported as a gap. It is never reported as zero.\n";

  return os.str();
}

std::string comparison_to_markdown(const session::ComparisonResult& cmp,
                                   const model::AnalysisResult* det) {
  std::ostringstream os;
  os << "# Comparison report\n\n";
  os << "**Overall verdict: `" << session::to_string(cmp.overall) << "`**\n\n";

  if (!cmp.incompatibilities.empty()) {
    os << "## Why this comparison cannot be used as a gate\n\n";
    for (const auto& i : cmp.incompatibilities) {
      os << "- " << escape_markdown(i) << "\n";
    }
    os << "\n";
    if (cmp.cross_platform) {
      os << "> This is a cross-platform pair. The two sides may be displayed "
            "together, but they do not share metric semantics and cannot drive "
            "a regression gate.\n\n";
    }
  }

  os << "## Conditions\n\n";
  os << "| Field | Baseline | Candidate |\n|---|---|---|\n";
  const auto& b = cmp.baseline.conditions;
  const auto& c = cmp.candidate.conditions;
  auto row = [&](const char* label, const std::string& x, const std::string& y) {
    os << "| " << label << " | " << (x.empty() ? "*unknown*" : escape_markdown(x))
       << " | " << (y.empty() ? "*unknown*" : escape_markdown(y)) << " |\n";
  };
  row("platform", b.platform, c.platform);
  row("scenario", b.scenario_id + " v" + b.scenario_version,
      c.scenario_id + " v" + c.scenario_version);
  row("device", b.device_model, c.device_model);
  row("device form", b.device_form, c.device_form);
  row("OS", b.os_version, c.os_version);
  row("refresh policy", b.refresh_policy, c.refresh_policy);
  row("collector preset", b.collector_preset, c.collector_preset);
  row("launch class", b.launch_class, c.launch_class);
  row("thermal", b.thermal_state, c.thermal_state);
  row("power", b.power_state, c.power_state);
  os << "| mode | " << model::to_string(cmp.baseline.mode) << " | "
     << model::to_string(cmp.candidate.mode) << " |\n";
  os << "| certified benchmark | "
     << (cmp.baseline.eligibility.certified_benchmark() ? "yes" : "**no**")
     << " | "
     << (cmp.candidate.eligibility.certified_benchmark() ? "yes" : "**no**")
     << " |\n\n";

  os << "## Metrics\n\n";
  os << "| Metric | Baseline median | Candidate median | Delta | Relative | "
        "Runs (b/c) | Verdict |\n|---|---|---|---|---|---|---|\n";
  for (const auto& m : cmp.metrics) {
    os << "| `" << escape_markdown(m.metric_name) << "` | "
       << num(m.baseline_median, m.unit) << " | " << num(m.candidate_median, m.unit)
       << " | " << num(m.absolute_delta, m.unit) << " | ";
    os << (m.relative_delta.has_value() ? pct(*m.relative_delta)
                                        : std::string("*undefined*"));
    os << " | " << m.baseline_valid_runs << "/" << m.candidate_valid_runs
       << " | `" << session::to_string(m.verdict) << "` |\n";
  }
  os << "\n";

  for (const auto& m : cmp.metrics) {
    if (m.reasons.empty() && m.excluded_runs.empty()) continue;
    os << "### `" << escape_markdown(m.metric_name) << "`\n\n";
    for (const auto& r : m.reasons) os << "- " << escape_markdown(r) << "\n";
    if (!m.excluded_runs.empty()) {
      os << "\n**Excluded runs**\n\n";
      for (const auto& e : m.excluded_runs) os << "- " << escape_markdown(e) << "\n";
    }
    os << "\n";
  }

  os << "## Thresholds\n\n";
  os << "- minimum absolute delta: " << cmp.thresholds.min_absolute_delta << "\n";
  os << "- minimum relative delta: " << pct(cmp.thresholds.min_relative_delta) << "\n";
  os << "- minimum valid runs per side: " << cmp.thresholds.min_valid_runs << "\n";
  os << "- maximum relative spread before inconclusive: "
     << pct(cmp.thresholds.max_relative_spread) << "\n\n";
  os << "A verdict requires **both** the absolute and the relative threshold to "
        "clear. A run that did not complete its scenario is excluded, never "
        "counted as a fast success.\n";

  if (det != nullptr) {
    os << "\n## Detector execution\n\n";
    for (const auto& note : det->data_quality_notes) {
      os << "- " << escape_markdown(note) << "\n";
    }
    if (!det->data_quality_notes.empty()) os << "\n";
    os << "| Detector | Outcome | Issues | Notes |\n|---|---|---|---|\n";
    for (const auto& run : det->rule_runs) {
      std::string notes;
      for (const auto& r : run.skipped_reasons) {
        if (!notes.empty()) notes += "<br>";
        notes += escape_markdown(r);
      }
      os << "| `" << run.rule_id << "` v" << run.rule_version << " | `"
         << model::to_string(run.outcome) << "` | " << run.issues_emitted
         << " | " << (notes.empty() ? "--" : notes) << " |\n";
    }
    os << "\n";
    if (det->issues.empty()) {
      os << "No detector that ran produced a finding over this comparison. "
            "That is not the same as the comparison being clean: the table "
            "above says which detectors ran.\n";
    } else {
      for (const auto& issue : det->issues) {
        write_issue(os, issue, /*include_source_paths=*/false);
      }
    }
  }
  return os.str();
}

}  // namespace mpi::report
