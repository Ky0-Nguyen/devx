// DET-09 -- wait / lock contention.
//
// The allowed conclusion is "wait observed; cause qualified", and the rule's
// own deferred note said the rest: naming a lock owner without proof would be
// a fabricated cause. So this rule separates two things that are easy to
// conflate.
//
// The wait is measured. The kernel says a thread was off the CPU and in which
// state, so "this thread waited 40 ms" is a fact.
//
// Who it waited *for* is not, and cannot be, read off a wait. What the trace
// does give is the thread that made it runnable again -- `sched_waking` is
// emitted by the waker itself. For a contended mutex the waker is usually the
// releaser, which makes it the strongest available evidence and still not
// proof: a thread can be woken by a timer, by unrelated I/O completing, or by
// whichever thread happened to signal a shared condition variable. So the
// waker is reported as a candidate, named as "the thread that unblocked it",
// and never as "the thread that held the lock".
//
// One more distinction the rule keeps: being *runnable* but not running is
// contention for the CPU, while *sleeping* is waiting for something else.
// Both are waits and they have different causes, so they are never summed.
#include <algorithm>
#include <map>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

std::string ms(model::TimeNs ns) {
  return std::to_string(ns / 1000000) + " ms";
}

bool payload_true(const model::Event& e, const char* key) {
  const auto* v = e.payload.find(key);
  return v != nullptr && v->is_bool() && v->as_bool();
}

std::string payload_text(const model::Event& e, const char* key) {
  const auto* v = e.payload.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

std::string thread_label(const model::NormalizedTrace& t,
                         const model::Event& e) {
  for (const auto& info : t.threads) {
    if (info.thread_instance_id != e.thread_instance_id) continue;
    if (!info.name.empty()) {
      return info.name + (info.is_main_ui_thread ? " (the UI main thread)" : "");
    }
  }
  const auto name = payload_text(e, "thread_name");
  if (!name.empty()) return name;
  return e.thread_instance_id;
}

class Det09 final : public Rule {
 public:
  std::string id() const override { return "DET-09"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "scheduling"; }
  std::string title() const override { return "Wait / lock contention"; }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"wait_intervals",
         "scheduling intervals with a state and a duration, so a wait is "
         "measured rather than inferred"},
        {"waker_identity",
         "for any claim about who unblocked a thread, a `sched_waking` event "
         "emitted by the waker itself"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_wait_ms", 16, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only, near one frame at 60 Hz; no "
                      "platform publishes a wait budget"},
        ThresholdSpec{"severity_high_ms", 100, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "impact ordering, not a standard"},
        ThresholdSpec{"severity_medium_ms", 32, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a thread sleeping because it has nothing to do is waiting, not "
        "contending, and this rule cannot always tell them apart",
        "the waker is not necessarily the holder: a thread can be woken by a "
        "timer, by unrelated I/O completing, or by whichever thread signalled "
        "a shared condition",
        "runnable-but-not-running on a busy emulator reflects the host's "
        "scheduler as much as the app's own contention",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    const bool any = std::any_of(
        t.events.begin(), t.events.end(), [](const model::Event& e) {
          return e.category == model::EventCategory::kSchedule &&
                 e.duration_ns.has_value();
        });
    if (!any) {
      unmet.push_back(
          "no scheduling intervals were collected. This needs `atrace` on "
          "Android, which is off by default because it traces the whole "
          "device: record with --scheduling");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const auto min_wait = static_cast<model::TimeNs>(
        ctx.override_for("DET-09.min_wait_ms").value_or(16.0) * 1000000.0);
    const auto high = static_cast<model::TimeNs>(
        ctx.override_for("DET-09.severity_high_ms").value_or(100.0) * 1000000.0);
    const auto medium = static_cast<model::TimeNs>(
        ctx.override_for("DET-09.severity_medium_ms").value_or(32.0) *
        1000000.0);

    // Waits are reported per thread and per state. Summing a CPU-contention
    // wait with a sleep would produce a number that means neither.
    struct Group {
      std::string state;
      std::string thread_instance_id;
      model::TimeNs total = 0;
      model::TimeNs longest = 0;
      const model::Event* worst = nullptr;
      std::int64_t count = 0;
    };
    std::map<std::string, Group> groups;

    for (const auto& e : t.events) {
      if (ctx.cancel.cancelled()) return;
      if (e.category != model::EventCategory::kSchedule) continue;
      if (!e.duration_ns.has_value()) continue;
      // "woken" is an instant, not a wait; and a dead thread is not waiting.
      if (e.name != "sleeping" && e.name != "uninterruptible" &&
          e.name != "runnable") {
        continue;
      }
      const std::string key = e.thread_instance_id + "\x1f" + e.name;
      auto& group = groups[key];
      group.state = e.name;
      group.thread_instance_id = e.thread_instance_id;
      group.total += *e.duration_ns;
      ++group.count;
      if (*e.duration_ns > group.longest) {
        group.longest = *e.duration_ns;
        group.worst = &e;
      }
    }

    for (const auto& [key, group] : groups) {
      if (ctx.cancel.cancelled()) return;
      static_cast<void>(key);
      if (group.worst == nullptr) continue;
      if (group.longest < min_wait) continue;

      // An I/O wait is DET-03's finding, not this one: reporting it here as
      // well would double-count the same block under two names.
      bool is_io = false;
      for (const auto& e : t.events) {
        if (e.category != model::EventCategory::kIo) continue;
        if (e.thread_instance_id != group.thread_instance_id) continue;
        if (!payload_true(e, "iowait")) continue;
        const model::TimeNs start = group.worst->timestamp;
        const model::TimeNs end = start + group.longest;
        if (e.timestamp + 1000000 >= start && e.timestamp <= end) is_io = true;
      }
      if (is_io) {
        out.record.skipped_reasons.push_back(
            "the longest " + group.state + " stretch on " +
            thread_label(t, *group.worst) +
            " was an I/O wait the kernel already explained, which DET-03 "
            "reports; it is not counted again here");
        continue;
      }

      // Who unblocked it, if the trace says so. This is evidence about the
      // *waker*, which is not the same claim as the holder of a lock.
      const model::Event* wake = nullptr;
      bool waker_in_this_app = false;
      for (const auto& e : t.events) {
        if (e.category != model::EventCategory::kSchedule) continue;
        if (e.name != "woken") continue;
        if (e.thread_instance_id != group.thread_instance_id) continue;
        const model::TimeNs end = group.worst->timestamp + group.longest;
        // The wake lands at the end of the wait it ended.
        if (e.timestamp < group.worst->timestamp) continue;
        if (e.timestamp > end + 2000000) continue;
        if (wake == nullptr || e.timestamp > wake->timestamp) {
          wake = &e;
          waker_in_this_app = payload_true(e, "waker_is_this_app");
        }
      }

      // Only threads the user waits on.
      //
      // A background thread sleeping is doing its job, and a wait on it
      // matters only if it blocks something the user is waiting for -- which
      // needs a dependency this provider does not give. Reporting them all
      // buried the real findings: a 64-thread app produced 23 findings in six
      // seconds, and the rule's own known false positives said most were
      // threads with nothing to do. Excluded threads are listed with the
      // reason rather than dropped silently.
      bool user_visible = false;
      for (const auto& info : t.threads) {
        if (info.thread_instance_id != group.thread_instance_id) continue;
        user_visible = info.is_main_ui_thread || info.is_js_thread;
        if (!user_visible && !info.name.empty()) {
          user_visible = info.name.rfind("mqt_", 0) == 0;
        }
        break;
      }
      if (!user_visible) {
        user_visible = payload_true(*group.worst, "is_main_thread");
      }
      if (!user_visible) {
        out.record.skipped_reasons.push_back(
            "a " + ms(group.longest) + " " + group.state + " stretch on " +
            thread_label(t, *group.worst) +
            " was not reported: it is not a thread the user waits on, and "
            "whether it delayed one cannot be established from thread states "
            "alone" +
            (waker_in_this_app
                 ? " (another thread in this app did unblock it)"
                 : ""));
        continue;
      }

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = t.session_id;
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      issue.start_ns = group.worst->timestamp;
      issue.end_ns = group.worst->timestamp + group.longest;
      issue.process_instance_id = group.worst->process_instance_id;
      issue.thread_instance_id = group.thread_instance_id;

      const std::string label = thread_label(t, *group.worst);
      const bool cpu_contention = group.state == "runnable";
      issue.title = label + (cpu_contention
                                 ? " was runnable but not running for "
                                 : " waited " ) +
                    ms(group.longest);
      if (!cpu_contention) issue.title += " in one " + group.state + " stretch";

      // The wait is measured. Everything about its cause is qualified.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "the kernel reported this thread " + group.state + " for " +
          ms(group.longest) + ", and " + ms(group.total) + " across " +
          std::to_string(group.count) +
          " stretch(es). The wait is measured; what it was waiting for is not";

      if (wake != nullptr) {
        const std::string waker = payload_text(*wake, "waker_name");
        const bool same_app = payload_true(*wake, "waker_is_this_app");
        // A named waker is the strongest evidence available here, which makes
        // it a candidate cause and not a demonstrated one.
        issue.cause_status = model::CauseStatus::kCandidate;
        issue.confidence_basis +=
            ". It was made runnable again by " +
            (waker.empty() ? std::string("another thread") : "'" + waker + "'") +
            (same_app ? " in this app" : " outside this app") +
            ", which is the thread that unblocked it -- not necessarily the "
            "thread that held what it was waiting for";
        issue.missing_evidence.push_back(
            "proof of what was held. The trace names the thread that "
            "unblocked this one; a lock's owner is a different claim and "
            "nothing here establishes it");
        if (!same_app) {
          issue.alternative_explanations.push_back(
              "the waker is outside this app, so this may be the system "
              "scheduling or a service responding rather than the app's own "
              "contention");
        }
      } else {
        issue.cause_status = model::CauseStatus::kUnknown;
        issue.missing_evidence.push_back(
            "who unblocked this thread: no `sched_waking` for it was "
            "captured, so not even a candidate can be named");
      }

      if (cpu_contention) {
        issue.missing_evidence.push_back(
            "what occupied the CPU instead. Runnable-but-not-running means "
            "the thread was ready and something else ran; this trace does "
            "not attribute which work displaced it");
      } else {
        issue.missing_evidence.push_back(
            "the app's own call stack at the wait: atrace gives thread states "
            "and not stacks, so where in the code it waited is unknown");
      }
      for (const auto& fp : known_false_positives()) {
        issue.alternative_explanations.push_back(fp);
      }
      issue.suggested_verification.push_back(
          "re-record with CPU sampling enabled and check what ran during this "
          "interval; a wait whose waker is in this app is worth reading "
          "alongside that thread's own work");

      if (group.longest >= high) {
        issue.severity = model::Severity::kMedium;
      } else {
        issue.severity = model::Severity::kLow;
      }
      if (group.longest >= medium && group.longest < high) {
        issue.severity = model::Severity::kLow;
      }
      issue.severity_rationale =
          "severity follows the longest single wait, and is capped at medium: "
          "the wait is measured but its cause is not, and a qualified finding "
          "should not outrank a measured one";

      issue.threshold_expression =
          "longest " + group.state + " stretch >= " + ms(min_wait);
      issue.threshold_origin =
          "configurable_heuristic: the default sits near one frame at 60 Hz, "
          "and no platform publishes a wait budget";

      model::Metric longest;
      longest.name = "schedule." + group.state + "_longest_ns";
      longest.unit = "ns";
      longest.value = static_cast<double>(group.longest);
      longest.provider = "atrace";
      longest.method = model::MetricMethod::kMeasured;
      longest.aggregation = "longest_single_interval";
      longest.app_scoped = true;
      longest.window_start_ns = issue.start_ns;
      longest.window_end_ns = issue.end_ns;
      longest.limitations.push_back(
          "this state is reported on its own and never added to another: "
          "runnable-but-not-running and sleeping are different waits with "
          "different causes");
      issue.metrics.push_back(std::move(longest));

      model::Metric total;
      total.name = "schedule." + group.state + "_total_ns";
      total.unit = "ns";
      total.value = static_cast<double>(group.total);
      total.provider = "atrace";
      total.method = model::MetricMethod::kDerived;
      total.aggregation = "sum_over_intervals_of_this_state_on_this_thread";
      total.app_scoped = true;
      total.limitations.push_back(
          "a sum of waits is not time the user waited: these stretches may "
          "overlap other threads' progress");
      issue.metrics.push_back(std::move(total));

      model::EvidenceRef ref;
      ref.kind = "event";
      ref.id = group.worst->event_id;
      ref.start_ns = issue.start_ns;
      ref.end_ns = issue.end_ns;
      ref.note = "the longest " + group.state + " stretch on this thread";
      ref.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(ref));

      if (wake != nullptr) {
        model::EvidenceRef waker_ref;
        waker_ref.kind = "event";
        waker_ref.id = wake->event_id;
        waker_ref.start_ns = wake->timestamp;
        waker_ref.note =
            "the wake that ended it, emitted by the waking thread itself";
        waker_ref.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(waker_ref));
      }

      issue.fingerprint = make_fingerprint(id(), version(),
                                           group.thread_instance_id,
                                           group.state);
      out.issues.push_back(std::move(issue));
    }
  }
};

}  // namespace

RulePtr make_det09_wait_contention() { return std::make_shared<Det09>(); }

}  // namespace mpi::rules
