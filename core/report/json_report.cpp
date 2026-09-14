#include "core/report/report.hpp"

namespace mpi::report {

std::string to_json(const model::NormalizedTrace& trace,
                    const model::AnalysisResult& analysis,
                    const ReportOptions& opts) {
  json::Value root = json::Value::object();
  root.set("schema_version", json::Value::string("2.0"));
  root.set("report_kind", json::Value::string("session_analysis"));

  // The trace summary comes first so a reader sees the measurement context
  // before any conclusion.
  root.set("trace", trace.to_json(opts.include_raw_events));

  json::Value an = analysis.to_json();
  if (!opts.include_suppressed) {
    json::Value kept = json::Value::array();
    const json::Value* issues = an.find("issues");
    if (issues && issues->is_array()) {
      for (const auto& i : issues->items()) {
        const json::Value* sup = i.find("suppression");
        const json::Value* flag = sup ? sup->find("suppressed") : nullptr;
        if (flag && flag->as_bool()) continue;
        kept.push_back(i);
      }
    }
    an.set("issues", std::move(kept));
    an.set("suppressed_issues_omitted", json::Value::boolean(true));
  }

  if (!opts.include_source_paths) {
    // Spec section 14: export excludes private source and symbol bundles by
    // default. Paths are redacted from the export, not from the local session.
    json::Value* issues = nullptr;
    for (auto& m : an.members()) {
      if (m.first == "issues") issues = &m.second;
    }
    if (issues && issues->is_array()) {
      for (auto& issue : issues->items()) {
        for (auto& m : issue.members()) {
          if (m.first != "candidate_stacks" || !m.second.is_array()) continue;
          for (auto& stack : m.second.items()) {
            for (auto& sm : stack.members()) {
              if (sm.first != "locations" || !sm.second.is_array()) continue;
              for (auto& loc : sm.second.items()) {
                loc.set("file", json::Value::null());
                loc.set("redacted",
                        json::Value::string(
                            "source path omitted from export; re-run with "
                            "--include-source-paths to include it"));
              }
            }
          }
        }
      }
    }
    an.set("source_paths_included", json::Value::boolean(false));
  } else {
    an.set("source_paths_included", json::Value::boolean(true));
  }

  root.set("analysis", std::move(an));
  return root.dump(2) + "\n";
}

std::string comparison_to_json(const session::ComparisonResult& cmp) {
  json::Value root = json::Value::object();
  root.set("schema_version", json::Value::string("2.0"));
  root.set("report_kind", json::Value::string("comparison"));
  root.set("comparison", cmp.to_json());
  return root.dump(2) + "\n";
}

}  // namespace mpi::report
