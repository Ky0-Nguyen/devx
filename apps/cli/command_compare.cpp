#include <fstream>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/report/report.hpp"

namespace mpi::cli {
namespace {

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v && v->is_string() ? v->as_string() : std::string();
}

// Reads a run-set file: the conditions plus one measurement per run.
bool load_run_set(const std::string& path, const std::string& label,
                  session::RunSet& out, std::string& error) {
  json::ParseError perr;
  auto parsed = json::parse_file(path, json::Limits{}, &perr);
  if (!parsed) {
    error = path + ": " + perr.message;
    return false;
  }
  const json::Value& root = *parsed;
  out.label = label;

  if (const json::Value* c = root.find("conditions"); c && c->is_object()) {
    auto& k = out.conditions;
    k.platform = str_at(*c, "platform");
    k.scenario_id = str_at(*c, "scenario_id");
    k.scenario_version = str_at(*c, "scenario_version");
    k.device_id = str_at(*c, "device_id");
    k.device_model = str_at(*c, "device_model");
    k.device_form = str_at(*c, "device_form");
    k.os_version = str_at(*c, "os_version");
    k.refresh_policy = str_at(*c, "refresh_policy");
    k.collector_preset = str_at(*c, "collector_preset");
    if (const json::Value* r = c->find("collector_sample_rate_hz");
        r && r->is_number()) {
      k.collector_sample_rate_hz = r->as_double();
    }
    k.launch_class = str_at(*c, "launch_class");
    k.thermal_state = str_at(*c, "thermal_state");
    k.power_state = str_at(*c, "power_state");
    k.input_data_version = str_at(*c, "input_data_version");
    k.account_state = str_at(*c, "account_state");
    k.network_condition = str_at(*c, "network_condition");
    k.cache_state = str_at(*c, "cache_state");
  } else {
    error = path + ": no \"conditions\" object; comparability cannot be checked";
    return false;
  }

  out.mode = model::measurement_mode_from_string(str_at(root, "measurement_mode"));

  // Eligibility can be supplied directly, or derived from a build profile.
  if (const json::Value* e = root.find("benchmark_eligibility");
      e && e->is_object()) {
    const std::string status = str_at(*e, "status");
    out.eligibility.status =
        status == "eligible" ? model::EligibilityStatus::kEligible
        : status == "ineligible" ? model::EligibilityStatus::kIneligible
                                 : model::EligibilityStatus::kInsufficientEvidence;
    if (const json::Value* rs = e->find("reasons"); rs && rs->is_array()) {
      for (const auto& r : rs->items()) {
        if (r.is_string()) out.eligibility.reasons.push_back(r.as_string());
      }
    }
    const json::Value* ov = e->find("user_override");
    out.eligibility.user_override = ov && ov->as_bool();
    out.eligibility.user_override_note = str_at(*e, "user_override_note");
  } else {
    // Absent eligibility is insufficient evidence, never a pass.
    out.eligibility.status = model::EligibilityStatus::kInsufficientEvidence;
    out.eligibility.reasons.push_back(
        "the run set did not state its benchmark eligibility");
  }

  const json::Value* runs = root.find("runs");
  if (!runs || !runs->is_array()) {
    error = path + ": no \"runs\" array";
    return false;
  }
  for (const auto& r : runs->items()) {
    if (!r.is_object()) continue;
    session::RunMeasurement m;
    m.session_id = str_at(r, "session_id");
    m.metric_name = str_at(r, "metric_name");
    m.unit = str_at(r, "unit");
    if (const json::Value* v = r.find("value"); v && v->is_number()) {
      m.value = v->as_double();
    }
    const json::Value* done = r.find("scenario_completed");
    // Absent means unknown, and an unknown scenario outcome cannot count as a
    // valid run (spec I15).
    m.scenario_completed = done ? done->as_bool() : false;
    if (!done) m.exclusion_reason = "scenario completion not stated";
    if (m.exclusion_reason.empty()) {
      m.exclusion_reason = str_at(r, "exclusion_reason");
    }
    const json::Value* warm = r.find("warm_up");
    m.warm_up = warm && warm->as_bool();
    if (m.metric_name.empty()) continue;
    out.runs.push_back(std::move(m));
  }
  return true;
}

}  // namespace

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
  if (!load_run_set(inv.positional[0], "baseline", baseline, error)) {
    std::cerr << "error: " << error << "\n";
    return ExitCode::kCollectionError;
  }
  if (!load_run_set(inv.positional[1], "candidate", candidate, error)) {
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

  const std::string format = inv.flag("format", inv.global.json ? "json" : "markdown");
  std::string rendered;
  if (format == "json") {
    rendered = report::comparison_to_json(result);
  } else if (format == "markdown" || format == "md") {
    rendered = report::comparison_to_markdown(result);
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
