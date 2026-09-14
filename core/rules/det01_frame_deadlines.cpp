// DET-01 -- missed frame deadlines / UI responsiveness.
//
// The deadline comes from the *observed* refresh rate, not from a constant.
// There is no universal 16 ms rule; a 120 Hz window has an 8.33 ms deadline
// and a variable-rate window has no single defensible deadline at all.
//
// A display-callback proxy source never produces an "observed" detection: the
// most it can support is "suspected", and the issue says so (spec E21).
#include <algorithm>
#include <cmath>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

class Det01 final : public Rule {
 public:
  std::string id() const override { return "DET-01"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "frames_responsiveness"; }
  std::string title() const override {
    return "Missed frame deadlines / UI responsiveness";
  }
  std::string delivery_phase() const override { return "M1"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"frame_records", "at least one frame record for the target process"},
        {"frame_deadline",
         "a per-frame deadline, either provider-reported or derived from an "
         "observed refresh rate"},
        {"presentation_or_labelled_proxy",
         "presentation timestamps, or a clearly labelled display-callback proxy"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_frames", 20, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "below this many frames the miss rate is too noisy to "
                      "report; raise it for steadier results"},
        ThresholdSpec{"miss_rate_medium", 0.05, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project, it is not a "
                      "platform standard"},
        ThresholdSpec{"miss_rate_high", 0.20, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project"},
        ThresholdSpec{"consecutive_miss_run", 3, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a run of consecutive misses is more likely to be "
                      "user-visible than the same count scattered"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "an idle surface legitimately producing few frames can look like a low "
        "frame rate without any jank (spec E03)",
        "a background surface's frames are not a foreground animation (E04)",
        "a display-callback proxy can report a late callback while the frame "
        "was still presented on time (E21)",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    if (t.frames.empty()) {
      unmet.push_back(
          "no frame records were collected; this is a missing collector, not "
          "an absence of jank");
      return unmet;
    }
    std::size_t with_deadline = 0;
    std::size_t with_presentation = 0;
    for (const auto& f : t.frames) {
      if (effective_deadline(t, f).has_value()) ++with_deadline;
      if (f.presented_ns.has_value()) ++with_presentation;
    }
    if (with_deadline == 0) {
      unmet.push_back(
          "no frame carries a deadline and no refresh rate was observed, so no "
          "deadline can be derived (there is no universal 16 ms rule)");
    }
    if (with_presentation == 0) {
      unmet.push_back(
          "no frame carries a presentation timestamp, so lateness cannot be "
          "measured");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override;

 private:
  // Prefers the provider's own deadline; falls back to the observed refresh
  // interval. Returns nullopt for a variable-rate window.
  static std::optional<model::TimeNs> effective_deadline(
      const model::NormalizedTrace& t, const model::FrameRecord& f) {
    if (f.deadline_ns.has_value()) return f.deadline_ns;
    if (const model::RefreshInterval* r = t.refresh_at(f.start_ns)) {
      return r->deadline_ns();
    }
    return std::nullopt;
  }
};

void Det01::evaluate(const RuleContext& ctx, RuleOutput& out) const {
  const auto& t = *ctx.trace;
  const auto th = thresholds();
  auto value_of = [&](const char* name) {
    for (const auto& x : th) {
      if (x.name == name) {
        return ctx.override_for(id() + std::string(".") + name).value_or(x.value);
      }
    }
    return 0.0;
  };
  const auto min_frames = static_cast<std::size_t>(value_of("min_frames"));
  const double miss_medium = value_of("miss_rate_medium");
  const double miss_high = value_of("miss_rate_high");
  const auto run_threshold = static_cast<int>(value_of("consecutive_miss_run"));

  // Group by (process, surface): a jank claim about one surface must not be
  // computed across a different layer (spec E19).
  struct Group {
    std::string process_instance_id;
    std::string surface;
    std::vector<const model::FrameRecord*> frames;
  };
  std::vector<Group> groups;
  for (const auto& f : t.frames) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
      return g.process_instance_id == f.process_instance_id && g.surface == f.surface;
    });
    if (it == groups.end()) {
      groups.push_back(Group{f.process_instance_id, f.surface, {&f}});
    } else {
      it->frames.push_back(&f);
    }
  }

  for (const auto& g : groups) {
    if (ctx.cancel.cancelled()) return;

    std::vector<const model::FrameRecord*> evaluable;
    std::size_t undeterminable = 0;
    bool any_proxy = false;
    bool any_presentation_truth = false;
    bool any_deadline_report = false;
    bool any_unknown_source = false;
    for (const auto* f : g.frames) {
      const auto deadline = effective_deadline(t, *f);
      if (!deadline.has_value() || !f->presented_ns.has_value()) {
        ++undeterminable;
        continue;
      }
      switch (f->source) {
        case model::FrameSource::kPresentationTimestamps:
          any_presentation_truth = true;
          break;
        case model::FrameSource::kFrameDeadlineReports:
          any_deadline_report = true;
          break;
        case model::FrameSource::kDisplayCallbackProxy:
          any_proxy = true;
          break;
        case model::FrameSource::kUnknown:
          any_unknown_source = true;
          break;
      }
      evaluable.push_back(f);
    }

    if (evaluable.size() < min_frames) {
      out.record.skipped_reasons.push_back(
          "surface '" + (g.surface.empty() ? std::string("(unnamed)") : g.surface) +
          "' had only " + std::to_string(evaluable.size()) +
          " evaluable frame(s), below the min_frames threshold of " +
          std::to_string(min_frames));
      continue;
    }

    std::vector<const model::FrameRecord*> missed;
    model::TimeNs worst_overrun = 0;
    const model::FrameRecord* worst = nullptr;
    int current_run = 0;
    int longest_run = 0;
    model::TimeNs run_start = 0;
    model::TimeNs run_end = 0;
    model::TimeNs best_run_start = 0;
    model::TimeNs best_run_end = 0;

    for (const auto* f : evaluable) {
      const auto deadline = *effective_deadline(t, *f);
      const model::TimeNs elapsed = *f->presented_ns - f->start_ns;
      const model::TimeNs overrun = elapsed - deadline;
      if (overrun > 0) {
        missed.push_back(f);
        if (current_run == 0) run_start = f->start_ns;
        run_end = *f->presented_ns;
        ++current_run;
        if (current_run > longest_run) {
          longest_run = current_run;
          best_run_start = run_start;
          best_run_end = run_end;
        }
        if (overrun > worst_overrun) {
          worst_overrun = overrun;
          worst = f;
        }
      } else {
        current_run = 0;
      }
    }

    if (missed.empty()) continue;

    const double miss_rate =
        static_cast<double>(missed.size()) / static_cast<double>(evaluable.size());

    model::Issue issue;
    issue.rule_id = id();
    issue.rule_version = version();
    issue.session_id = t.session_id;
    issue.category = category();
    issue.process_instance_id = g.process_instance_id;
    issue.mode = ctx.mode;
    issue.eligibility = ctx.eligibility;

    const std::string surface_label =
        g.surface.empty() ? std::string("the target surface") : "surface '" + g.surface + "'";
    issue.title = std::to_string(missed.size()) + " of " +
                  std::to_string(evaluable.size()) + " frames missed their deadline on " +
                  surface_label;

    // Detection status hangs on the frame source, not on the numbers. There
    // are three genuinely different qualities of evidence here and collapsing
    // them would either overstate a proxy or understate a real platform
    // measurement.
    if (any_presentation_truth && !any_proxy && !any_unknown_source) {
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "presentation timestamps and a per-frame deadline were both present, "
          "so lateness is measured rather than inferred";
    } else if (any_deadline_report && !any_proxy && !any_unknown_source) {
      // The platform reported both the deadline and when the frame finished.
      // That is a measurement, so the miss is observed -- but finishing late
      // is not the same event as presenting late, and the issue says so.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "the platform supplied both the per-frame deadline and the frame's "
          "completion time, so the overrun is measured; completion is not the "
          "same instant as presentation, so this is a missed completion "
          "deadline rather than a proven late frame on screen";
      issue.alternative_explanations.push_back(
          "the compositor may still have presented the frame on time despite "
          "the app finishing it late");
      issue.missing_evidence.push_back(
          "actual presentation timestamps (the platform reported none for "
          "these frames)");
    } else if (any_proxy) {
      issue.detection_status = model::DetectionStatus::kSuspected;
      issue.confidence_basis =
          "frame timing came from a display-callback proxy, which is not "
          "guaranteed presentation truth; a late callback does not prove a "
          "late frame";
      issue.alternative_explanations.push_back(
          "the display callback ran late while the frame still presented on "
          "time");
      issue.missing_evidence.push_back("actual presentation timestamps");
    } else {
      issue.detection_status = model::DetectionStatus::kSuspected;
      issue.confidence_basis =
          "the frame source did not identify itself, so the measurement cannot "
          "be treated as presentation truth";
      issue.missing_evidence.push_back("a declared frame timing source");
    }

    // The cause is never established by this rule: it observes the symptom.
    issue.cause_status = model::CauseStatus::kUnknown;
    issue.missing_evidence.push_back(
        "what occupied the UI thread during the missed frames (needs CPU "
        "samples or scheduling evidence on the same clock domain)");
    issue.alternative_explanations.push_back(
        "GPU-side or compositor cost outside this app's CPU work");
    issue.alternative_explanations.push_back(
        "contention from another process or a system service");
    issue.suggested_verification.push_back(
        "re-record the same interaction with CPU sampling and scheduling "
        "collectors enabled, then check which thread was running during the "
        "missed interval");

    if (miss_rate >= miss_high || longest_run >= run_threshold * 2) {
      issue.severity = model::Severity::kHigh;
    } else if (miss_rate >= miss_medium || longest_run >= run_threshold) {
      issue.severity = model::Severity::kMedium;
    } else {
      issue.severity = model::Severity::kLow;
    }
    issue.severity_rationale =
        "severity reflects measured miss rate and the longest consecutive run; "
        "it is an impact ordering, not a confidence in any cause";

    issue.threshold_expression =
        "miss_rate >= " + std::to_string(miss_medium) + " (medium) / " +
        std::to_string(miss_high) + " (high), or a run of " +
        std::to_string(run_threshold) + "+ consecutive misses";
    issue.threshold_origin =
        "configurable_heuristic for the rates; the per-frame deadline itself "
        "is platform_deadline_observed, derived from the observed refresh rate";

    // Report the interval of the worst consecutive run, so clicking the issue
    // lands on real evidence rather than the whole capture.
    issue.start_ns = longest_run > 0 ? best_run_start : missed.front()->start_ns;
    issue.end_ns = longest_run > 0
                       ? best_run_end
                       : missed.back()->presented_ns.value_or(missed.back()->start_ns);

    // Attach a screen only if a marker actually covers this interval.
    for (const auto& m : t.markers) {
      if (m.kind != "screen_mount" && m.kind != "navigation_end") continue;
      if (m.timestamp_ns <= issue.start_ns) issue.screen = m.screen;
    }

    model::Metric rate;
    rate.name = "frames.missed_deadline_rate";
    rate.unit = "fraction";
    rate.value = miss_rate;
    rate.provider = t.frames.front().event_id.empty() ? "unknown" : "frame_source";
    rate.process_instance_id = g.process_instance_id;
    rate.app_scoped = false;
    rate.window_start_ns = issue.start_ns;
    rate.window_end_ns = issue.end_ns;
    rate.method = model::MetricMethod::kMeasured;
    rate.aggregation = "ratio_over_evaluable_frames";
    if (undeterminable > 0) {
      rate.limitations.push_back(
          std::to_string(undeterminable) +
          " frame(s) lacked a deadline or a presentation timestamp and are "
          "excluded from the denominator rather than counted as on time");
    }
    if (any_proxy) {
      rate.limitations.push_back(
          "includes frames timed by a display-callback proxy");
    }
    issue.metrics.push_back(std::move(rate));

    model::Metric worst_metric;
    worst_metric.name = "frames.worst_overrun_ns";
    worst_metric.unit = "ns";
    worst_metric.value = static_cast<double>(worst_overrun);
    worst_metric.provider = "frame_source";
    worst_metric.process_instance_id = g.process_instance_id;
    worst_metric.window_start_ns = issue.start_ns;
    worst_metric.window_end_ns = issue.end_ns;
    worst_metric.method = model::MetricMethod::kMeasured;
    worst_metric.aggregation = "max";
    issue.metrics.push_back(std::move(worst_metric));

    model::Metric run_metric;
    run_metric.name = "frames.longest_consecutive_miss_run";
    run_metric.unit = "count";
    run_metric.value = static_cast<double>(longest_run);
    run_metric.provider = "frame_source";
    run_metric.process_instance_id = g.process_instance_id;
    run_metric.window_start_ns = issue.start_ns;
    run_metric.window_end_ns = issue.end_ns;
    run_metric.method = model::MetricMethod::kMeasured;
    run_metric.aggregation = "max";
    issue.metrics.push_back(std::move(run_metric));

    // Evidence: the worst frame plus up to nine more, each resolvable.
    if (worst) {
      model::EvidenceRef e;
      e.kind = "frame";
      e.id = worst->event_id;
      e.start_ns = worst->start_ns;
      e.end_ns = worst->presented_ns;
      e.note = "worst overrun: " +
               std::to_string(worst_overrun / 1000000) + " ms past deadline";
      e.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(e));
    }
    std::size_t attached = 0;
    for (const auto* f : missed) {
      if (attached >= 9) break;
      if (worst && f == worst) continue;
      model::EvidenceRef e;
      e.kind = "frame";
      e.id = f->event_id;
      e.start_ns = f->start_ns;
      e.end_ns = f->presented_ns;
      e.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(e));
      ++attached;
    }

    if (const model::Coverage* cov = t.coverage_for("frame_source")) {
      if (cov->covered_fraction() < 0.95) {
        issue.quality_flags.push_back(model::QualityFlag::kProviderDropped);
        issue.missing_evidence.push_back(
            "frame collector covered only " +
            std::to_string(static_cast<int>(cov->covered_fraction() * 100)) +
            "% of the window");
      }
    }
    if (t.synthetic) issue.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
    if (t.partial) {
      issue.quality_flags.push_back(model::QualityFlag::kIncompleteSpan);
      issue.missing_evidence.push_back(
          "the capture is partial: " +
          (t.partial_reasons.empty() ? std::string("reason not stated")
                                     : t.partial_reasons.front()));
    }

    issue.proposed_remediation.push_back(
        "identify the work on the UI thread during the flagged interval before "
        "changing anything; this rule reports the symptom, not its cause");

    issue.occurrence_count = static_cast<std::int64_t>(missed.size());
    issue.fingerprint = make_fingerprint(
        id(), version(), g.process_instance_id + "|" + g.surface,
        "missed_frame_deadlines");
    issue.issue_id = issue.fingerprint;
    out.issues.push_back(std::move(issue));
  }
}

}  // namespace

RulePtr make_det01_frame_deadlines() { return std::make_shared<Det01>(); }

}  // namespace mpi::rules
