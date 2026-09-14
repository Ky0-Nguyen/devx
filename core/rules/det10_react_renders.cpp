// DET-10 -- repeated React renders.
//
// The spec's allowed conclusion is unusually explicit: "Pattern, not
// automatically defect" (section 10.2). A component that commits many times
// may be doing exactly what it should -- a list that streams rows, a progress
// indicator, a screen animating a value. So this rule reports a *pattern* and
// says in the finding that it is not a defect, and its severity never rises
// above low.
//
// It also refuses a substitute. React commit counts come from React's own
// profiling data, which the app reports through the SDK. A sampled stack that
// happens to be inside React is not the same measurement: sampling says where
// time went, not how many times a component committed, and the spec requires
// the profiling data rather than the samples (section 11).
#include <algorithm>
#include <map>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

struct Commits {
  std::string component;
  std::string screen;
  std::string clock_domain;
  std::int64_t count = 0;
  model::TimeNs first_ns = 0;
  model::TimeNs last_ns = 0;
  model::TimeNs total_duration_ns = 0;
  bool have_duration = false;
  std::vector<const model::Marker*> markers;
};

class Det10 final : public Rule {
 public:
  std::string id() const override { return "DET-10"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "react"; }
  std::string title() const override { return "Repeated React renders"; }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"react_commit_data",
         "React commit markers from the app's own profiling data, reported "
         "through the SDK; sampled stacks are not a substitute"},
        {"component_identity",
         "each commit must name the component it belongs to"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_commits", 20, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only. There is no correct number of "
                      "renders: a streaming list legitimately commits far "
                      "more than this"},
        ThresholdSpec{"min_commits_per_second", 10, "count_per_second",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "the rate matters more than the total, since a long "
                      "session accumulates commits honestly"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a list that streams rows, a progress indicator, or an animated value "
        "commits often by design, and this rule cannot tell that from a "
        "redundant re-render",
        "a component that commits cheaply many times can cost less than one "
        "that commits once expensively; a count is not a cost",
        "commits driven by real incoming data are not the component's fault",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    const bool any_commit =
        std::any_of(t.markers.begin(), t.markers.end(),
                    [](const model::Marker& m) { return m.kind == "react_commit"; });
    if (!any_commit) {
      // Deliberately specific about the substitute this rule will not accept.
      unmet.push_back(
          "no React commit data was collected. This needs the app to report "
          "React's own profiling output through the SDK (`reactCommit`); CPU "
          "samples that land inside React measure where time went, not how "
          "many times a component committed, and are not a substitute");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const double min_commits =
        ctx.override_for("DET-10.min_commits").value_or(20.0);
    const double min_rate =
        ctx.override_for("DET-10.min_commits_per_second").value_or(10.0);

    // Grouped by component, and by screen within it: the same component on
    // two screens is two different render stories.
    std::map<std::string, Commits> grouped;
    for (const auto& m : t.markers) {
      if (m.kind != "react_commit") continue;
      const std::string component =
          m.interaction.empty() ? std::string("(unnamed component)")
                                : m.interaction;
      const std::string key = m.screen + "\x1f" + component;
      auto& entry = grouped[key];
      if (entry.count == 0) {
        entry.component = component;
        entry.screen = m.screen;
        entry.first_ns = m.timestamp_ns;
        entry.clock_domain = m.clock_domain;
      }
      // The app may report a batch of commits in one marker.
      std::int64_t commits = 1;
      if (const auto* payload = m.payload.find("commit_count")) {
        if (payload->is_number() && payload->as_int() > 0) {
          commits = payload->as_int();
        }
      }
      entry.count += commits;
      entry.last_ns = std::max(entry.last_ns, m.timestamp_ns);
      if (m.duration_ns.has_value()) {
        entry.total_duration_ns += *m.duration_ns;
        entry.have_duration = true;
      }
      entry.markers.push_back(&m);
    }

    for (const auto& [key, entry] : grouped) {
      if (ctx.cancel.cancelled()) return;
      static_cast<void>(key);
      if (static_cast<double>(entry.count) < min_commits) {
        out.record.skipped_reasons.push_back(
            "component '" + entry.component + "' committed " +
            std::to_string(entry.count) + " time(s), below min_commits");
        continue;
      }
      const double span_s =
          entry.last_ns > entry.first_ns
              ? static_cast<double>(entry.last_ns - entry.first_ns) / 1e9
              : 0.0;
      if (span_s <= 0.0) {
        out.record.skipped_reasons.push_back(
            "component '" + entry.component +
            "' reported all its commits at one instant, so no rate can be "
            "derived from them");
        continue;
      }
      const double rate = static_cast<double>(entry.count) / span_s;
      if (rate < min_rate) {
        out.record.skipped_reasons.push_back(
            "component '" + entry.component + "' committed " +
            std::to_string(entry.count) + " time(s) over " +
            std::to_string(span_s) +
            "s, which is below the rate threshold: a long session "
            "accumulates commits honestly");
        continue;
      }

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = t.session_id;
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      issue.screen = entry.screen;
      // The commits are stamped by the app. The interval is placed on the
      // capture's timeline so clicking the issue lands on the right part of
      // the capture, and says so when it cannot be.
      const auto interval = map_producer_interval(t, entry.clock_domain,
                                                  entry.first_ns, entry.last_ns);
      issue.start_ns = interval.start_ns;
      issue.end_ns = interval.end_ns;

      issue.title = entry.component + " committed " +
                    std::to_string(entry.count) + " times in " +
                    std::to_string(static_cast<int>(span_s * 1000)) + " ms";

      // The count is measured, so the pattern is observed. Whether it is a
      // problem is not measured at all, and the rule says so in the same
      // breath rather than leaving severity to imply it.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "the commit count comes from React's own profiling data, so the "
          "render pattern is measured. Whether it is a defect is not: a "
          "component can legitimately commit this often, and this rule "
          "reports the pattern rather than a fault";
      issue.cause_status = model::CauseStatus::kUnknown;

      issue.missing_evidence.push_back(
          "why each commit happened: React's profiling data gives counts, not "
          "the prop or state change that triggered them");
      if (!entry.have_duration) {
        issue.missing_evidence.push_back(
            "the cost of each commit. The app reported counts without "
            "durations, so nothing here says this pattern was expensive");
      }
      issue.missing_evidence.push_back(
          "a frame record covering the same interval, which is what would "
          "show whether these commits cost the user anything");
      if (!interval.mapped) {
        issue.missing_evidence.push_back(
            "a measured mapping from the app's clock ('" + interval.domain +
            "') to the capture's timeline: this interval is on the app's own "
            "clock and cannot be lined up against device measurements");
      }
      for (const auto& fp : known_false_positives()) {
        issue.alternative_explanations.push_back(fp);
      }
      issue.suggested_verification.push_back(
          "check whether the commits coincide with incoming data or with a "
          "changing prop that does not need to change, then re-record the "
          "same interaction and compare the frame timing");

      // Never above low: the spec's conclusion for this rule is a pattern,
      // and a pattern must not outrank a measured problem in the list.
      issue.severity = model::Severity::kLow;
      issue.severity_rationale =
          "capped at low on purpose: this is a render pattern, not a "
          "demonstrated defect, and it must not outrank a measured frame or "
          "CPU finding";

      issue.threshold_expression =
          "commits >= " + std::to_string(static_cast<int>(min_commits)) +
          " and rate >= " + std::to_string(static_cast<int>(min_rate)) + "/s";
      issue.threshold_origin =
          "configurable_heuristic: there is no correct number of renders, and "
          "these are starting values to tune per app";

      model::Metric count;
      count.name = "react.commits";
      count.unit = "count";
      count.value = static_cast<double>(entry.count);
      count.provider = "app SDK (React profiling)";
      count.method = model::MetricMethod::kMeasured;
      count.aggregation = "sum_over_component_on_screen";
      count.app_scoped = true;
      count.window_start_ns = entry.first_ns;
      count.window_end_ns = entry.last_ns;
      count.limitations.push_back(
          "a count is not a cost: this says how often the component "
          "committed, not what it cost");
      issue.metrics.push_back(std::move(count));

      if (entry.have_duration) {
        model::Metric cost;
        cost.name = "react.commit_duration_total_ns";
        cost.unit = "ns";
        cost.value = static_cast<double>(entry.total_duration_ns);
        cost.provider = "app SDK (React profiling)";
        cost.method = model::MetricMethod::kMeasured;
        cost.aggregation = "sum_of_reported_commit_durations";
        cost.app_scoped = true;
        cost.limitations.push_back(
            "the app's own measurement of its commits, not the platform's, "
            "and it does not include what the commit caused downstream");
        issue.metrics.push_back(std::move(cost));
      }

      for (const auto* marker : entry.markers) {
        if (issue.evidence.size() >= 8) break;
        model::EvidenceRef ref;
        ref.kind = "marker";
        ref.id = marker->event_id;
        ref.start_ns = marker->timestamp_ns;
        ref.note = "React commit reported by the app";
        ref.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(ref));
      }

      issue.fingerprint =
          make_fingerprint(id(), version(), entry.screen, entry.component);
      out.issues.push_back(std::move(issue));
    }
  }
};

}  // namespace

RulePtr make_det10_react_renders() { return std::make_shared<Det10>(); }

}  // namespace mpi::rules
