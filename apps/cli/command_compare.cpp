#include <fstream>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"

namespace mpi::cli {

ExitCode cmd_compare(const Invocation& inv) {
  if (inv.positional.size() < 2) {
    std::cerr << "error: compare needs a baseline and a candidate run-set "
                 "file\n\n"
                 "A run-set file looks like:\n"
                 "  {\n"
                 "    \"conditions\": {\"platform\": \"android\", "
                 "\"scenario_id\": \"checkout\", \"scenario_version\": \"3\", "
                 "\"device_form\": \"physical\", ...},\n"
                 "    \"measurement_mode\": \"benchmark\",\n"
                 "    \"benchmark_eligibility\": {\"status\": \"eligible\"},\n"
                 "    \"runs\": [{\"session_id\": \"s1\", \"metric_name\": "
                 "\"startup.first_frame_ns\", \"unit\": \"ns\", \"value\": "
                 "412000000, \"scenario_completed\": true}]\n"
                 "  }\n";
    return ExitCode::kUsage;
  }

  session::RunSet baseline;
  session::RunSet candidate;
  std::string error;
  if (!session::read_run_set(inv.positional[0], "baseline", baseline, error)) {
    std::cerr << "error: " << error << "\n";
    return ExitCode::kCollectionError;
  }
  if (!session::read_run_set(inv.positional[1], "candidate", candidate, error)) {
    std::cerr << "error: " << error << "\n";
    return ExitCode::kCollectionError;
  }

  session::ComparisonThresholds th;
  if (inv.has_flag("min-runs")) {
    const int v = std::atoi(inv.flag("min-runs").c_str());
    if (v < 1) {
      std::cerr << "error: --min-runs must be at least 1\n";
      return ExitCode::kUsage;
    }
    th.min_valid_runs = static_cast<std::size_t>(v);
  }
  if (inv.has_flag("min-relative-delta")) {
    th.min_relative_delta = std::atof(inv.flag("min-relative-delta").c_str());
  }
  if (inv.has_flag("min-absolute-delta")) {
    th.min_absolute_delta = std::atof(inv.flag("min-absolute-delta").c_str());
  }
  if (inv.has_flag("max-spread")) {
    th.max_relative_spread = std::atof(inv.flag("max-spread").c_str());
  }

  const auto result =
      session::compare(std::move(baseline), std::move(candidate), th);

  // DET-08 turns the comparison into a finding with the issue contract around
  // it: what was measured, what stays unknown, and how far the claim reaches.
  // The verdict alone cannot say any of that.
  rules::EngineOptions rule_opts;
  rule_opts.mode = result.candidate.mode;
  rule_opts.cancel = inv.global.cancel;
  const auto detection = rules::analyze_comparison(
      session::to_regression_input(result), rule_opts);

  const std::string format = inv.flag("format", inv.global.json ? "json" : "markdown");
  std::string rendered;
  if (format == "json") {
    rendered = report::comparison_to_json(result, &detection);
  } else if (format == "markdown" || format == "md") {
    rendered = report::comparison_to_markdown(result, &detection);
  } else {
    std::cerr << "error: --format must be json or markdown\n";
    return ExitCode::kUsage;
  }

  const std::string out_path = inv.flag("out");
  if (out_path.empty()) {
    std::cout << rendered;
  } else {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot write " << out_path << "\n";
      return ExitCode::kCollectionError;
    }
    f.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
  }

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  // Distinct exit codes so CI can branch on the verdict rather than on text.
  switch (result.overall) {
    case session::ComparisonVerdict::kRegression:
      return ExitCode::kRegressionDetected;
    case session::ComparisonVerdict::kInconclusive:
      return ExitCode::kInconclusive;
    case session::ComparisonVerdict::kImprovement:
    case session::ComparisonVerdict::kNoSignificantChange:
      return ExitCode::kOk;
  }
  return ExitCode::kInconclusive;
}

}  // namespace mpi::cli
