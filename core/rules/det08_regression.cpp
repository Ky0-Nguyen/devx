// DET-08 -- performance regression between a baseline and a candidate.
//
// This is the one detector whose evidence is two *run sets* rather than one
// capture, so it runs over the comparison engine's result. It does not decide
// significance itself: `core/session/compare` already weighs run counts,
// spread, exclusions and condition mismatches, and duplicating that judgement
// here would give the report a second opinion nobody tested.
//
// What the rule adds is the issue contract: what was measured, what is still
// unknown, and -- the part a bare verdict cannot express -- how far the claim
// reaches. Two diagnostic runs can show a real difference between themselves
// while certifying nothing about release performance (spec I16).
#include <algorithm>
#include <cmath>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

std::string number(double v) {
  // Trimmed fixed notation: a regression report is read by people, and
  // "0.120000" next to "12.0%" is noise.
  std::string s = std::to_string(v);
  while (s.size() > 1 && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string percent(double fraction) {
  return number(std::round(fraction * 1000.0) / 10.0) + "%";
}

class Det08 final : public Rule {
 public:
  std::string id() const override { return "DET-08"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "regression"; }
  std::string title() const override { return "Performance regression"; }
  std::string delivery_phase() const override { return "M4"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"run_set_pair",
         "a baseline and a candidate run set for the same scenario version"},
        {"comparable_conditions",
         "matching device, OS, refresh policy, collector preset, launch class "
         "and input state"},
        {"sufficient_valid_runs",
         "enough completed runs on both sides for a median and a spread"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"severity_high_relative_delta", 0.20, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a regression at or above this relative size is ranked "
                      "high; it orders impact and is not a platform standard"},
        ThresholdSpec{"severity_medium_relative_delta", 0.10, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a real difference between two configurations that says nothing about "
        "release performance, when either side is not benchmark-eligible "
        "(spec I16)",
        "an environmental change the run conditions do not capture -- a "
        "background update, a thermal state that drifted between the two "
        "sets -- looks identical to a code regression at this level (I04, I06)",
        "a scenario that changed meaning between the two versions while "
        "keeping its id, which no amount of run repetition detects",
    };
  }

  bool uses_comparison_input() const override { return true; }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    if (ctx.regression == nullptr) {
      unmet.push_back(
          "no baseline/candidate pair was supplied: a regression is a "
          "difference between two run sets, so this rule runs over "
          "`mpi compare` and not over a single session");
      return unmet;
    }
    const auto& in = *ctx.regression;
    if (in.metrics.empty()) {
      unmet.push_back("the comparison carried no metric at all");
      return unmet;
    }
    if (in.cross_platform) {
      // Refusing here rather than reporting a qualified finding: the metrics
      // do not mean the same thing on the two platforms, so there is no
      // honest severity to attach (spec I19).
      unmet.push_back(
          "the pair spans two platforms. Android and iOS metrics do not share "
          "semantics, so the pair may be displayed side by side but cannot "
          "drive a regression gate");
    }
    for (const auto& reason : in.incompatibilities) {
      unmet.push_back("the run conditions are not comparable: " + reason);
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& in = *ctx.regression;
    const double high = ctx.override_for("DET-08.severity_high_relative_delta")
                            .value_or(0.20);
    const double medium =
        ctx.override_for("DET-08.severity_medium_relative_delta").value_or(0.10);

    std::size_t regressions = 0;
    for (const auto& m : in.metrics) {
      if (ctx.cancel.cancelled()) return;

      // Everything that is not a regression is still accounted for: a metric
      // that came back inconclusive is not a metric that came back clean.
      if (m.verdict != "regression") {
        std::string note = "metric '" + m.metric_name + "': " + m.verdict;
        if (!m.reasons.empty()) {
          note += " (" + m.reasons.front();
          for (std::size_t i = 1; i < m.reasons.size(); ++i) {
            note += "; " + m.reasons[i];
          }
          note += ")";
        }
        out.record.skipped_reasons.push_back(std::move(note));
        continue;
      }
      ++regressions;

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;

      const std::string scenario =
          in.scenario_id.empty()
              ? std::string("the scenario")
              : "'" + in.scenario_id + "' v" + in.scenario_version;

      issue.title = m.metric_name + " regressed on " + scenario;
      if (m.relative_delta.has_value()) {
        issue.title += " by " + percent(*m.relative_delta);
      }

      // The difference itself is measured: two medians over completed runs,
      // both past the absolute and relative thresholds, with spread inside
      // the limit. What it is *about* is the part that needs qualifying.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "both sides completed " + std::to_string(m.baseline_valid_runs) +
          " and " + std::to_string(m.candidate_valid_runs) +
          " valid runs of the same scenario version under conditions the "
          "comparison found compatible, and the difference cleared both the "
          "absolute and the relative threshold with spread inside the "
          "allowed limit";

      // A regression is a difference, not a cause. Nothing here identifies
      // the change responsible, and the rule does not pretend the candidate's
      // diff is evidence.
      issue.cause_status = model::CauseStatus::kUnknown;
      issue.missing_evidence.push_back(
          "a capture from a candidate run showing which work grew: this "
          "comparison has run-level timings only, no profile");
      issue.suggested_verification.push_back(
          "record a diagnostic session on both builds for the same scenario "
          "and compare the attributed work, then re-run this comparison after "
          "a candidate fix to confirm the number moves back");
      issue.alternative_explanations.push_back(
          "an environmental difference the run conditions do not record, such "
          "as a background task or a thermal state that drifted between the "
          "two sets");

      if (!m.certified) {
        // The measurement stands; the claim's reach does not. Kept as
        // missing evidence rather than a softer detection status, because
        // downgrading a measured difference would misdescribe what happened.
        issue.missing_evidence.push_back(
            "benchmark eligibility on both sides: this pair measures a real "
            "difference between these two configurations and certifies "
            "nothing about release performance");
        issue.alternative_explanations.push_back(
            "overhead from a debug build or an enabled collector, which can "
            "differ between the two sides without the shipped code changing");
      }

      const auto note_evidence = [&](const std::string& text) {
        model::EvidenceRef ref;
        ref.kind = "metric";
        ref.id = m.metric_name;
        ref.note = text;
        issue.evidence.push_back(std::move(ref));
      };
      for (const auto& reason : m.reasons) note_evidence(reason);
      for (const auto& excluded : m.excluded_runs) {
        note_evidence("excluded run: " + excluded);
      }
      if (m.baseline_spread.has_value() && m.candidate_spread.has_value()) {
        note_evidence("spread (interquartile) baseline " +
                      number(*m.baseline_spread) + " " + m.unit +
                      ", candidate " + number(*m.candidate_spread) + " " +
                      m.unit + "; reported instead of a single number because "
                      "a median alone hides how repeatable the runs were");
      }
      note_evidence("no tail percentile is reported: " +
                    std::to_string(m.candidate_valid_runs) +
                    " runs cannot support one (spec I18)");

      const double rel = m.relative_delta.value_or(0.0);
      if (rel >= high) {
        issue.severity = model::Severity::kHigh;
      } else if (rel >= medium) {
        issue.severity = model::Severity::kMedium;
      } else {
        issue.severity = model::Severity::kLow;
      }
      issue.severity_rationale =
          "severity follows the relative size of the change, which orders "
          "impact between regressions; it is not a confidence in any cause "
          "and not a statement about user-visible harm";

      issue.threshold_expression =
          "relative delta >= " + number(in.min_relative_delta) +
          " and absolute delta >= " + number(in.min_absolute_delta) + " over " +
          std::to_string(in.min_valid_runs) +
          "+ valid runs per side, with relative spread <= " +
          number(in.max_relative_spread);
      issue.threshold_origin =
          "project_budget for the comparison thresholds, which the caller "
          "sets per scenario; configurable_heuristic for the severity bands";

      // Two run sets have no shared timeline, so there is no interval to
      // focus. Leaving the window at zero is honest; inventing one would
      // point the UI at evidence that does not exist.
      issue.start_ns = 0;
      issue.end_ns = 0;

      model::Metric baseline_metric;
      baseline_metric.name = m.metric_name + ".baseline_median";
      baseline_metric.unit = m.unit;
      baseline_metric.value = m.baseline_median;
      baseline_metric.provider = "run_set:" + in.baseline_label;
      baseline_metric.method = model::MetricMethod::kDerived;
      baseline_metric.aggregation = "median_over_valid_runs";
      baseline_metric.app_scoped = true;
      issue.metrics.push_back(std::move(baseline_metric));

      model::Metric candidate_metric;
      candidate_metric.name = m.metric_name + ".candidate_median";
      candidate_metric.unit = m.unit;
      candidate_metric.value = m.candidate_median;
      candidate_metric.provider = "run_set:" + in.candidate_label;
      candidate_metric.method = model::MetricMethod::kDerived;
      candidate_metric.aggregation = "median_over_valid_runs";
      candidate_metric.app_scoped = true;
      issue.metrics.push_back(std::move(candidate_metric));

      if (m.absolute_delta.has_value()) {
        model::Metric delta;
        delta.name = m.metric_name + ".delta";
        delta.unit = m.unit;
        delta.value = m.absolute_delta;
        delta.provider = "comparison";
        delta.method = model::MetricMethod::kDerived;
        delta.aggregation = "candidate_median_minus_baseline_median";
        delta.app_scoped = true;
        issue.metrics.push_back(std::move(delta));
      }

      issue.fingerprint = make_fingerprint(
          id(), version(), in.scenario_id + "@" + in.scenario_version,
          m.metric_name);
      out.issues.push_back(std::move(issue));
    }

    // No summary line is needed when nothing regressed: `run()` records the
    // outcome as ran-and-found-nothing, and every metric that could not be
    // decided was listed above rather than counted as unchanged.
    static_cast<void>(regressions);
  }
};

}  // namespace

RulePtr make_det08_regression() { return std::make_shared<Det08>(); }

}  // namespace mpi::rules
