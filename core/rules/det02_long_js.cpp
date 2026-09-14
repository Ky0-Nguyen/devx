// DET-02 -- long JS execution.
//
// The conclusion this rule is allowed to reach is narrow: long JS work was
// observed. Whether it harmed the UI is a separate question that needs a
// measured mapping between the JS clock and the UI clock plus frame evidence
// in the same interval. Without that mapping the issue says the UI effect is
// unestablished, and the 50 ms threshold is labelled as our configurable
// heuristic rather than a mobile OS standard (spec section 10.3).
#include <algorithm>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

class Det02 final : public Rule {
 public:
  std::string id() const override { return "DET-02"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "js_execution"; }
  std::string title() const override { return "Long JS execution"; }
  std::string delivery_phase() const override { return "M1"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"js_tasks",
         "JS execution spans with measured durations (a sampling profile alone "
         "does not contain task boundaries)"},
        {"js_clock_domain", "a declared clock domain for the JS spans"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"long_task_ms", 50, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "an initial heuristic for a task long enough to be worth "
                      "looking at; it is not a platform deadline and not a "
                      "mobile OS standard"},
        ThresholdSpec{"severe_task_ms", 250, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "an initial heuristic for a task likely to be noticed "
                      "regardless of frame evidence"},
        ThresholdSpec{"min_tasks", 1, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a single long task is reportable; unlike a rate, it "
                      "needs no sample population"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a long task on a background JS runtime or worker may do no UI harm "
        "(spec E07, G06)",
        "a task measured while a debugger was paused has an invalid duration "
        "(spec C08) -- the eligibility model records the pause, but a "
        "mid-capture attach can still contaminate one interval",
        "startup bundle evaluation is expected to be long and is not by itself "
        "a defect",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    std::size_t with_duration = 0;
    for (const auto& j : t.js_tasks) {
      if (j.duration_ns.has_value()) ++with_duration;
    }
    if (t.js_tasks.empty()) {
      // Say which of the two situations applies: no JS collector at all, or a
      // sampling profile that legitimately has no task boundaries.
      const bool has_js_samples = std::any_of(
          t.cpu_samples.begin(), t.cpu_samples.end(), [&](const model::CpuSample& s) {
            const model::ThreadInfo* ti = t.thread(s.thread_instance_id);
            return ti && ti->is_js_thread;
          });
      if (has_js_samples) {
        unmet.push_back(
            "only JS sampling data is present; a sampling profile has no task "
            "boundaries, so task duration cannot be derived from it");
      } else {
        unmet.push_back(
            "no JS execution spans were collected; this is a missing collector, "
            "not an absence of long JS work");
      }
      return unmet;
    }
    if (with_duration == 0) {
      unmet.push_back(
          "every JS span is missing its duration (unterminated spans); "
          "durations remain unknown rather than estimated");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override;
};

void Det02::evaluate(const RuleContext& ctx, RuleOutput& out) const {
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
  const auto long_ns = static_cast<model::TimeNs>(value_of("long_task_ms") * 1e6);
  const auto severe_ns = static_cast<model::TimeNs>(value_of("severe_task_ms") * 1e6);

  // Group long tasks by name so repeated occurrences of the same work become
  // one issue with an occurrence count (spec H08).
  struct Group {
    std::string name;
    std::string process_instance_id;
    std::string thread_instance_id;
    std::vector<const model::JsTask*> tasks;
  };
  std::vector<Group> groups;
  std::size_t skipped_incomplete = 0;

  for (const auto& j : t.js_tasks) {
    if (ctx.cancel.cancelled()) return;
    if (!j.duration_ns.has_value()) {
      ++skipped_incomplete;
      continue;
    }
    if (*j.duration_ns < long_ns) continue;
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
      return g.name == j.name && g.thread_instance_id == j.thread_instance_id;
    });
    if (it == groups.end()) {
      groups.push_back(Group{j.name, j.process_instance_id, j.thread_instance_id, {&j}});
    } else {
      it->tasks.push_back(&j);
    }
  }

  if (skipped_incomplete > 0) {
    out.record.skipped_reasons.push_back(
        std::to_string(skipped_incomplete) +
        " JS span(s) had no measured duration and were not evaluated");
  }

  for (const auto& g : groups) {
    const model::JsTask* worst = *std::max_element(
        g.tasks.begin(), g.tasks.end(),
        [](const model::JsTask* a, const model::JsTask* b) {
          return a->duration_ns.value_or(0) < b->duration_ns.value_or(0);
        });
    const model::TimeNs worst_ns = worst->duration_ns.value_or(0);

    model::TimeNs total_ns = 0;
    for (const auto* x : g.tasks) total_ns += x->duration_ns.value_or(0);

    model::Issue issue;
    issue.rule_id = id();
    issue.rule_version = version();
    issue.session_id = t.session_id;
    issue.category = category();
    issue.process_instance_id = g.process_instance_id;
    issue.thread_instance_id = g.thread_instance_id;
    issue.mode = ctx.mode;
    issue.eligibility = ctx.eligibility;
    issue.start_ns = worst->start_ns;
    issue.end_ns = worst->start_ns + worst_ns;

    const std::string task_label = g.name.empty() ? "an unnamed JS task" : "'" + g.name + "'";
    issue.title = "Long JS execution: " + task_label + " ran " +
                  std::to_string(worst_ns / 1000000) + " ms";

    // The observation is about JS duration and nothing more.
    issue.detection_status = model::DetectionStatus::kObserved;
    issue.cause_status = model::CauseStatus::kUnknown;

    // Whether this harmed the UI depends entirely on clock mapping plus frames.
    const bool mapped = worst->clock_mapped_to_ui;
    bool frames_in_interval = false;
    bool missed_in_interval = false;
    if (mapped) {
      for (const auto& f : t.frames) {
        if (f.start_ns < issue.start_ns || f.start_ns > issue.end_ns) continue;
        frames_in_interval = true;
        const auto missed = f.missed_deadline();
        if (missed.has_value() && *missed) {
          missed_in_interval = true;
          model::EvidenceRef e;
          e.kind = "frame";
          e.id = f.event_id;
          e.start_ns = f.start_ns;
          e.end_ns = f.presented_ns;
          e.note = "frame overlapping the long JS task that missed its deadline";
          e.synthetic = t.synthetic;
          issue.evidence.push_back(std::move(e));
        }
      }
    }

    if (!mapped) {
      issue.confidence_basis =
          "the JS span duration is measured on the JS clock, but no measured "
          "mapping to the UI clock exists in this capture, so no statement "
          "about UI impact is derivable";
      issue.missing_evidence.push_back(
          "a measured mapping between the JS clock domain '" + worst->clock_domain +
          "' and the UI clock domain '" + t.primary_clock_domain + "'");
      issue.missing_evidence.push_back(
          "frame evidence in the same interval on a common clock");
      issue.quality_flags.push_back(model::QualityFlag::kClockUnmapped);
      issue.alternative_explanations.push_back(
          "the task may have run on a background runtime or while no frames "
          "were required, in which case there was no user-visible effect");
    } else if (missed_in_interval) {
      // Correlation, explicitly not causation (spec E20, section 0.8).
      issue.cause_status = model::CauseStatus::kCandidate;
      issue.confidence_basis =
          "the long JS task and one or more missed frame deadlines overlap on a "
          "measured common clock; this is a temporal correlation, which is a "
          "candidate cause and not a demonstrated one";
      issue.alternative_explanations.push_back(
          "a third factor (thermal state, contention from another process, GPU "
          "cost) could have caused both the long task and the missed frames");
      issue.missing_evidence.push_back(
          "a controlled experiment showing the frames recover when this task is "
          "shortened");
      issue.suggested_verification.push_back(
          "shorten or defer this task, re-record the identical scenario, and "
          "compare; a single improved run is not proof (spec section 10.3)");
    } else if (frames_in_interval) {
      issue.confidence_basis =
          "the long JS task overlaps frames that met their deadlines, so no "
          "user-visible frame harm was measured in this interval";
      issue.alternative_explanations.push_back(
          "the work may still delay a later interaction without dropping a frame");
    } else {
      issue.confidence_basis =
          "the clocks are mapped but no frames were recorded in the interval, "
          "so UI impact is unmeasured rather than absent";
      issue.missing_evidence.push_back("frame records inside the flagged interval");
    }

    if (worst_ns >= severe_ns) {
      issue.severity = model::Severity::kHigh;
    } else if (missed_in_interval) {
      issue.severity = model::Severity::kMedium;
    } else {
      issue.severity = model::Severity::kLow;
    }
    issue.severity_rationale =
        "severity is ordered by measured duration and whether overlapping "
        "frames missed their deadlines; it is not a confidence in the cause";

    issue.threshold_expression = "duration >= " +
                                 std::to_string(value_of("long_task_ms")) +
                                 " ms (severe at " +
                                 std::to_string(value_of("severe_task_ms")) + " ms)";
    issue.threshold_origin =
        "configurable_heuristic: this project's initial default, not a platform "
        "or OS standard";

    for (const auto& m : t.markers) {
      if (m.timestamp_ns > issue.start_ns) continue;
      if (m.kind == "screen_mount" || m.kind == "navigation_end") issue.screen = m.screen;
      if (m.kind == "interaction") issue.interaction = m.interaction;
    }

    model::Metric worst_m;
    worst_m.name = "js.task_duration_ns";
    worst_m.unit = "ns";
    worst_m.value = static_cast<double>(worst_ns);
    worst_m.provider = "js_spans";
    worst_m.process_instance_id = g.process_instance_id;
    worst_m.thread_instance_id = g.thread_instance_id;
    worst_m.window_start_ns = issue.start_ns;
    worst_m.window_end_ns = issue.end_ns;
    worst_m.method = model::MetricMethod::kMeasured;
    worst_m.aggregation = "max";
    if (!mapped) {
      worst_m.limitations.push_back(
          "measured on the JS clock domain with no mapping to the UI clock");
    }
    issue.metrics.push_back(std::move(worst_m));

    model::Metric total_m;
    total_m.name = "js.task_duration_total_ns";
    total_m.unit = "ns";
    total_m.value = static_cast<double>(total_ns);
    total_m.provider = "js_spans";
    total_m.process_instance_id = g.process_instance_id;
    total_m.thread_instance_id = g.thread_instance_id;
    total_m.window_start_ns = t.window_start_ns;
    total_m.window_end_ns = t.window_end_ns;
    total_m.method = model::MetricMethod::kMeasured;
    total_m.aggregation = "sum_over_occurrences";
    total_m.limitations.push_back(
        "sum of sibling task durations; nested spans are not included, so this "
        "is not a share of wall time");
    issue.metrics.push_back(std::move(total_m));

    for (const auto* x : g.tasks) {
      if (issue.evidence.size() >= 12) break;
      model::EvidenceRef e;
      e.kind = "js_task";
      e.id = x->event_id;
      e.start_ns = x->start_ns;
      e.end_ns = x->start_ns + x->duration_ns.value_or(0);
      e.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(e));
    }

    // Source attribution, only when a symbol artifact actually matched.
    if (ctx.symbol_service && !g.name.empty()) {
      model::SourceLocation loc = ctx.symbol_service->resolve_frame(g.name);
      issue.symbol_status = loc.symbol_status;
      if (loc.symbol_status != "unavailable") {
        model::CandidateStack stack;
        stack.frames = {g.name};
        stack.inclusive = false;
        stack.locations.push_back(std::move(loc));
        stack.note =
            "the span name as reported by the JS collector, resolved through "
            "the supplied source map";
        issue.candidate_stacks.push_back(std::move(stack));
      }
      if (issue.symbol_status != "exact_build_match") {
        issue.missing_evidence.push_back(
            "a source map bound to the exact captured bundle (current status: " +
            issue.symbol_status + ")");
      }
    } else {
      issue.missing_evidence.push_back("a JS source map for the captured bundle");
    }

    if (t.synthetic) issue.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);

    issue.proposed_remediation.push_back(
        "move the work off the JS thread, break it into yielding chunks, or "
        "defer it past the interaction -- after confirming from the evidence "
        "which of those the measurement actually supports");

    issue.occurrence_count = static_cast<std::int64_t>(g.tasks.size());
    issue.fingerprint = make_fingerprint(
        id(), version(), g.thread_instance_id, g.name.empty() ? "unnamed" : g.name);
    issue.issue_id = issue.fingerprint;
    out.issues.push_back(std::move(issue));
  }
}

}  // namespace

RulePtr make_det02_long_js() { return std::make_shared<Det02>(); }

}  // namespace mpi::rules
