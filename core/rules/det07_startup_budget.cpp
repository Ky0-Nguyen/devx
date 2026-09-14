// DET-07 -- startup budget exceedance.
//
// Two things make this rule different from the others, and both are about
// refusing to supply a number nobody chose.
//
// There is no default budget. A startup budget is a product decision -- what
// this app, on this device class, in this market, considers acceptable -- and
// no platform publishes one. So the rule skips until a budget is configured,
// and says that the budget is what is missing rather than the data.
//
// And there is no single startup endpoint. `am start -W`'s TotalTime ends when
// the activity reported being drawn; the platform's `Displayed` log line marks
// the first frame; neither is the moment the app became usable. Each endpoint
// is evaluated on its own and named in the finding, because averaging two
// different definitions produces a number that measures neither.
#include <algorithm>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

// The marker kinds a startup measurement arrives as, with what each endpoint
// actually means. Anything else is not a startup interval.
struct Endpoint {
  const char* marker_kind;
  const char* description;
  const char* qualification;
};

const Endpoint kEndpoints[] = {
    {"app_launch",
     "the activity reported being drawn",
     "this endpoint ends when the activity reported being drawn, which is "
     "earlier than the app becoming interactive: work started during startup "
     "can still be running"},
    {"startup_displayed",
     "the platform logged the first frame as displayed",
     "a displayed first frame can still be a placeholder or a skeleton "
     "screen; it is not proof that content was ready"},
};

std::string ms(model::TimeNs ns) {
  return std::to_string(ns / 1000000) + " ms";
}

class Det07 final : public Rule {
 public:
  std::string id() const override { return "DET-07"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "startup"; }
  std::string title() const override { return "Startup budget exceedance"; }
  std::string delivery_phase() const override { return "M4"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"startup_interval",
         "a startup interval from a launch this tool performed, with a named "
         "endpoint"},
        {"configured_budget",
         "a project startup budget: there is no platform standard to default "
         "to, so one must be configured as DET-07.budget_ms"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        // Deliberately zero: a zero budget is not a default, it is the
        // absence of one, and the rule refuses to run until it is set.
        ThresholdSpec{"budget_ms", 0, "ms", ThresholdOrigin::kProjectBudget,
                      "the project's own startup budget; no default exists "
                      "because no platform publishes one, so the rule does "
                      "not run until this is configured"},
        ThresholdSpec{"severity_high_overshoot", 0.50, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "exceeding the budget by this much or more is ranked "
                      "high; it orders impact, it is not a standard"},
        ThresholdSpec{"severity_medium_overshoot", 0.20, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a single launch is one sample. A cold start on a device that was "
        "busy with something else looks identical to a slow app (spec "
        "section 12 requires repeated runs before a startup claim)",
        "a debug build's startup includes work the shipped build does not do, "
        "and this rule never subtracts an estimate for it",
        "an emulator's startup is not a phone's: the CPU, the storage and the "
        "thermal behaviour all differ",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;

    const bool have_interval = std::any_of(
        t.markers.begin(), t.markers.end(), [](const model::Marker& m) {
          if (!m.duration_ns.has_value() || *m.duration_ns <= 0) return false;
          for (const auto& e : kEndpoints) {
            if (m.kind == e.marker_kind) return true;
          }
          return false;
        });
    if (!have_interval) {
      unmet.push_back(
          "no startup interval was recorded: this rule reads a launch this "
          "tool performed, so record with --launch. A capture of an app that "
          "was already running contains no startup to measure");
    }

    const auto budget = ctx.override_for("DET-07.budget_ms");
    if (!budget.has_value() || *budget <= 0.0) {
      unmet.push_back(
          "no startup budget is configured. A budget is a product decision "
          "and no platform publishes one, so this rule will not invent a "
          "number: set it with --threshold DET-07.budget_ms=<ms>");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const double budget_ms = ctx.override_for("DET-07.budget_ms").value_or(0.0);
    const auto budget_ns = static_cast<model::TimeNs>(budget_ms * 1000000.0);
    const double high =
        ctx.override_for("DET-07.severity_high_overshoot").value_or(0.50);
    const double medium =
        ctx.override_for("DET-07.severity_medium_overshoot").value_or(0.20);

    // One measurement per endpoint the capture recorded.
    struct Measured {
      const Endpoint* endpoint;
      const model::Marker* marker;
      model::TimeNs duration;
    };
    std::vector<Measured> measured;
    for (const auto& endpoint : kEndpoints) {
      for (const auto& marker : t.markers) {
        if (marker.kind != endpoint.marker_kind) continue;
        if (!marker.duration_ns.has_value() || *marker.duration_ns <= 0) continue;
        measured.push_back(Measured{&endpoint, &marker, *marker.duration_ns});
      }
    }

    // Endpoints that agree on the figure are one finding with two witnesses,
    // not two findings. On Android they routinely do agree, because the
    // platform derives its Displayed line from the same measurement -- and
    // two identically worded issues would read as two separate problems.
    std::vector<std::vector<const Measured*>> groups;
    for (const auto& m : measured) {
      auto it = std::find_if(groups.begin(), groups.end(),
                             [&](const std::vector<const Measured*>& g) {
                               return g.front()->duration == m.duration;
                             });
      if (it == groups.end()) {
        groups.push_back({&m});
      } else {
        it->push_back(&m);
      }
    }

    for (const auto& group : groups) {
      if (ctx.cancel.cancelled()) return;
      const model::TimeNs duration = group.front()->duration;

      std::string endpoint_names;
      for (const auto* m : group) {
        if (!endpoint_names.empty()) endpoint_names += " and ";
        endpoint_names += m->endpoint->marker_kind;
      }

      if (duration <= budget_ns) {
        for (const auto* m : group) {
          out.record.skipped_reasons.push_back(
              std::string("endpoint '") + m->endpoint->marker_kind +
              "' measured " + ms(duration) +
              ", within the configured budget of " + ms(budget_ns));
        }
        continue;
      }

      const model::Marker& marker = *group.front()->marker;
      const std::string reported_class =
          marker.payload.find("launch_class_reported") != nullptr
              ? marker.payload.find("launch_class_reported")->as_string()
              : std::string();

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = t.session_id;
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      issue.start_ns = marker.timestamp_ns;
      issue.end_ns = marker.timestamp_ns + duration;
      issue.process_instance_id = marker.process_instance_id;

      issue.title = "startup took " + ms(duration) + " against a " +
                    ms(budget_ns) + " budget";
      if (!reported_class.empty()) issue.title += " (" + reported_class + ")";
      issue.title += ", measured at " + endpoint_names;

      issue.detection_status = model::DetectionStatus::kObserved;
      std::string basis = "the interval is the platform's own figure";
      if (group.size() > 1) {
        basis += ", reported identically by " + std::to_string(group.size()) +
                 " endpoints";
      }
      basis += ", compared against a budget this project configured";
      issue.confidence_basis = basis;
      issue.cause_status = model::CauseStatus::kUnknown;

      issue.missing_evidence.push_back(
          "what the time was spent on: this is a launch duration, not a "
          "profile of the launch");
      issue.missing_evidence.push_back(
          "repeated launches. One launch is one sample, and a startup claim "
          "needs a set of comparable runs (spec section 12)");
      for (const auto* m : group) {
        issue.alternative_explanations.push_back(m->endpoint->qualification);
      }
      issue.alternative_explanations.push_back(
          "device state outside the app: storage pressure, a cold page cache, "
          "or another process holding the CPU during this launch");
      if (!ctx.eligibility.certified_benchmark()) {
        issue.missing_evidence.push_back(
            "benchmark eligibility: this session cannot certify release "
            "startup, and the figure is not corrected for build overhead");
      }
      issue.suggested_verification.push_back(
          "record five cold launches of the same scenario version and compare "
          "the medians with `mpi compare`, rather than acting on this single "
          "run");

      const double overshoot =
          budget_ns > 0 ? static_cast<double>(duration - budget_ns) /
                              static_cast<double>(budget_ns)
                        : 0.0;
      if (overshoot >= high) {
        issue.severity = model::Severity::kHigh;
      } else if (overshoot >= medium) {
        issue.severity = model::Severity::kMedium;
      } else {
        issue.severity = model::Severity::kLow;
      }
      issue.severity_rationale =
          "severity follows how far past the configured budget this launch "
          "ran; it orders impact and says nothing about the cause";

      issue.threshold_expression =
          "startup > " + ms(budget_ns) + " (DET-07.budget_ms)";
      issue.threshold_origin =
          "project_budget: configured by this project, not derived from any "
          "platform standard";
      issue.baseline_value = budget_ms;

      for (const auto* m : group) {
        const std::string provider =
            m->marker->payload.find("provider") != nullptr
                ? m->marker->payload.find("provider")->as_string()
                : std::string("the platform");

        model::Metric metric;
        metric.name = std::string("startup.") + m->endpoint->marker_kind + "_ns";
        metric.unit = "ns";
        metric.value = static_cast<double>(m->duration);
        metric.provider = provider;
        metric.method = model::MetricMethod::kMeasured;
        metric.aggregation = "single_launch";
        metric.app_scoped = true;
        metric.window_start_ns = m->marker->timestamp_ns;
        metric.window_end_ns = m->marker->timestamp_ns + m->duration;
        metric.process_instance_id = m->marker->process_instance_id;
        metric.limitations.push_back(m->endpoint->qualification);
        metric.limitations.push_back(
            "one launch; not an aggregate and not a baseline");
        issue.metrics.push_back(std::move(metric));

        model::EvidenceRef ref;
        ref.kind = "marker";
        ref.id = m->marker->event_id;
        ref.start_ns = m->marker->timestamp_ns;
        ref.end_ns = m->marker->timestamp_ns + m->duration;
        ref.note = std::string("endpoint: ") + m->endpoint->description +
                   " (" + provider + ")";
        ref.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(ref));
      }

      issue.fingerprint = make_fingerprint(
          id(), version(), t.target.app.app_identifier, endpoint_names);
      out.issues.push_back(std::move(issue));
    }
  }
};

}  // namespace

RulePtr make_det07_startup_budget() { return std::make_shared<Det07>(); }

}  // namespace mpi::rules
