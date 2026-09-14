// Report generation. JSON, Markdown, and the UI must agree (spec H12), so
// both writers render from the same AnalysisResult and the same trace.
#pragma once

#include <string>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/session/compare.hpp"

namespace mpi::report {

struct ReportOptions {
  // Export excludes private source and symbol bundles by default
  // (spec section 14).
  bool include_source_paths = false;
  bool include_raw_events = false;
  bool include_suppressed = true;
};

// The complete machine-readable report.
std::string to_json(const model::NormalizedTrace& trace,
                    const model::AnalysisResult& analysis,
                    const ReportOptions& opts);

// The human-readable report. Every value that came from a fixture is labelled,
// and anything the analysis could not determine says so rather than omitting
// the row.
std::string to_markdown(const model::NormalizedTrace& trace,
                        const model::AnalysisResult& analysis,
                        const ReportOptions& opts);

// Comparison report.
// `det` carries DET-08's findings over the same comparison. It is optional so
// a caller that only wants the numbers can omit it, but `mpi compare` always
// passes it: a verdict without the issue contract around it loses what the
// difference does and does not establish.
std::string comparison_to_json(const session::ComparisonResult& cmp,
                               const model::AnalysisResult* det = nullptr);
std::string comparison_to_markdown(const session::ComparisonResult& cmp,
                                   const model::AnalysisResult* det = nullptr);

// Escapes text for safe embedding in a Markdown table cell or body
// (spec section 15: escape exported HTML/Markdown).
std::string escape_markdown(const std::string& in);

}  // namespace mpi::report
