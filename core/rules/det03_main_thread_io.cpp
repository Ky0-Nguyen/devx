// DET-03 -- synchronous I/O on a thread the user is waiting for.
//
// The spec's minimum evidence is an I/O event plus a thread, with a stack for
// location, and the allowed conclusion is "I/O observed; severity based on
// impact" (section 10.2). Two things about the provider decide how this rule
// behaves.
//
// A `D` state is not proof of I/O. Uninterruptible sleep is usually disk, but
// it is also how a thread waits on some kernel locks and on page faults that
// are not file-backed. The kernel's own `iowait=1` on a
// `sched_blocked_reason` is the only thing here that says "this was I/O", so
// this rule requires it and will not promote a bare `D` state.
//
// And there is no stack. atrace gives a kernel `caller=` symbol, not the app's
// call stack, so a finding can say *that* the thread blocked reading and
// roughly where in the kernel -- never which line of app code did it. The
// enclosing userspace slice is used when the app emitted one, because that is
// the closest thing to a location this provider offers, and its absence is
// reported rather than filled in.
//
// "Main thread" is also not one thing in React Native. A block on the UI
// thread and a block on the JS thread are both user-visible and are different
// facts, so the finding names the thread it happened on and never calls a JS
// thread the main thread.
#include <algorithm>

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

// A thread whose blocking the user waits on. React Native has three of them
// and they are not interchangeable: the UI main thread, the JS thread, and
// the native-modules thread that JS calls into. A block on each has a
// different consequence, so the finding names which one it was.
struct BlockingThread {
  bool relevant = false;
  bool is_main = false;
  bool is_js = false;
  bool is_native_modules = false;
  std::string name;
  std::string role;
};

BlockingThread classify(const model::NormalizedTrace& t,
                        const std::string& thread_instance_id,
                        const model::Event& e) {
  BlockingThread out;
  for (const auto& info : t.threads) {
    if (info.thread_instance_id != thread_instance_id) continue;
    out.name = info.name;
    out.is_main = info.is_main_ui_thread;
    out.is_js = info.is_js_thread;
    break;
  }
  if (out.name.empty()) out.name = payload_text(e, "thread_name");
  // The collector marks the main thread from the platform's own convention
  // (tid == pid). The payload carries the same fact per event.
  if (payload_true(e, "is_main_thread")) out.is_main = true;
  // The names React Native's own threads actually carry. `mqt_v_js` runs
  // JavaScript; `mqt_v_native` is the native-modules queue JS calls into.
  // Treating the second as the first would put a block in the wrong place.
  const std::string n = out.name;
  if (!out.is_js) {
    out.is_js = n == "mqt_js" || n == "mqt_v_js" || n == "hermes";
  }
  out.is_native_modules = n == "mqt_native_modu" || n == "mqt_v_native" ||
                          n.rfind("mqt_native", 0) == 0;

  out.relevant = out.is_main || out.is_js || out.is_native_modules;
  if (out.is_main) {
    out.role = "the UI main thread";
  } else if (out.is_js) {
    out.role = "the JavaScript thread";
  } else if (out.is_native_modules) {
    out.role = "the native-modules thread";
  } else {
    out.role = "a thread";
  }
  return out;
}

class Det03 final : public Rule {
 public:
  std::string id() const override { return "DET-03"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "io"; }
  std::string title() const override {
    return "Synchronous I/O on a user-visible thread";
  }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"io_wait_evidence",
         "a kernel-reported I/O wait (`sched_blocked_reason` with iowait=1) "
         "attributed to one of the app's threads"},
        {"blocked_interval",
         "the uninterruptible interval that wait belongs to, so the block has "
         "a duration and not just an instant"},
        {"user_visible_thread",
         "the blocked thread must be the UI main thread or the JS thread; a "
         "background thread blocking on I/O is usually its job"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_block_ms", 8, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only. Chosen to be near a frame at "
                      "120 Hz, not because any platform publishes an I/O "
                      "budget"},
        ThresholdSpec{"severity_high_ms", 100, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a block this long is very likely to be user-visible; "
                      "still an impact ordering, not a standard"},
        ThresholdSpec{"severity_medium_ms", 24, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; tune per project"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a page-cache read that would have been served from memory on a "
        "warmer device: a cold first run blocks where a later one does not",
        "an I/O wait during startup, when the app is legitimately reading its "
        "own code and assets",
        "an emulator's I/O behaviour is not a phone's, and neither is a "
        "device with a full disk",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;

    const bool any_io = std::any_of(
        t.events.begin(), t.events.end(), [](const model::Event& e) {
          return e.category == model::EventCategory::kIo &&
                 payload_true(e, "iowait");
        });
    if (!any_io) {
      const bool any_schedule = std::any_of(
          t.events.begin(), t.events.end(), [](const model::Event& e) {
            return e.category == model::EventCategory::kSchedule;
          });
      if (any_schedule) {
        // The distinction that matters: the collector ran and the kernel
        // reported no I/O wait, which is not the same as no collector.
        unmet.push_back(
            "scheduling evidence was collected and none of it was an I/O "
            "wait: no `sched_blocked_reason` with iowait=1 was attributed to "
            "this app's threads. An uninterruptible state on its own is not "
            "promoted to an I/O claim");
      } else {
        unmet.push_back("no scheduling or I/O evidence was collected. " +
                        rules::scheduling_advice(*ctx.trace));
      }
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const auto min_block = static_cast<model::TimeNs>(
        ctx.override_for("DET-03.min_block_ms").value_or(8.0) * 1000000.0);
    const auto high = static_cast<model::TimeNs>(
        ctx.override_for("DET-03.severity_high_ms").value_or(100.0) * 1000000.0);
    const auto medium = static_cast<model::TimeNs>(
        ctx.override_for("DET-03.severity_medium_ms").value_or(24.0) *
        1000000.0);

    for (const auto& io : t.events) {
      if (ctx.cancel.cancelled()) return;
      if (io.category != model::EventCategory::kIo) continue;
      if (!payload_true(io, "iowait")) continue;

      const auto thread = classify(t, io.thread_instance_id, io);
      if (!thread.relevant) {
        out.record.skipped_reasons.push_back(
            "an I/O wait on thread '" +
            (thread.name.empty() ? io.thread_instance_id : thread.name) +
            "' was not reported: blocking on I/O is what a background thread "
            "is for, and this rule is about threads the user waits on");
        continue;
      }

      // The uninterruptible interval this wait belongs to. The reason event is
      // an instant; the interval is what gives it a duration, and without one
      // the block is real but its cost is unknown.
      const model::Event* interval = nullptr;
      for (const auto& candidate : t.events) {
        if (candidate.category != model::EventCategory::kSchedule) continue;
        if (candidate.thread_instance_id != io.thread_instance_id) continue;
        if (candidate.name != "uninterruptible") continue;
        if (!candidate.duration_ns.has_value()) continue;
        const model::TimeNs start = candidate.timestamp;
        const model::TimeNs end = start + *candidate.duration_ns;
        // The kernel emits the reason as the thread goes off CPU, so the
        // instant sits at or just before the interval it explains.
        if (io.timestamp + 1000000 < start || io.timestamp > end) continue;
        if (interval == nullptr ||
            *candidate.duration_ns > *interval->duration_ns) {
          interval = &candidate;
        }
      }

      if (interval == nullptr) {
        out.record.skipped_reasons.push_back(
            "an I/O wait on " + thread.role +
            " had no uninterruptible interval around it, so how long it "
            "blocked is unknown; the wait is real but its cost is not "
            "measurable from this trace");
        continue;
      }
      const model::TimeNs duration = *interval->duration_ns;
      if (duration < min_block) {
        out.record.skipped_reasons.push_back(
            "an I/O wait on " + thread.role + " lasted " + ms(duration) +
            ", below the min_block threshold");
        continue;
      }

      // The app's own slice covering the block, which is the closest thing to
      // a location this provider offers.
      const model::Event* slice = nullptr;
      for (const auto& candidate : t.events) {
        if (candidate.category != model::EventCategory::kOther) continue;
        if (candidate.thread_instance_id != io.thread_instance_id) continue;
        if (!candidate.duration_ns.has_value()) continue;
        if (candidate.timestamp > interval->timestamp) continue;
        if (candidate.timestamp + *candidate.duration_ns < interval->timestamp) {
          continue;
        }
        if (slice == nullptr || candidate.timestamp > slice->timestamp) {
          slice = &candidate;  // the innermost enclosing slice
        }
      }

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = t.session_id;
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      issue.start_ns = interval->timestamp;
      issue.end_ns = interval->timestamp + duration;
      issue.process_instance_id = io.process_instance_id;
      issue.thread_instance_id = io.thread_instance_id;

      issue.title = thread.role + " blocked " + ms(duration) + " on I/O";
      if (!thread.name.empty()) issue.title += " (" + thread.name + ")";
      if (slice != nullptr) issue.title += " during " + slice->name;

      // The kernel reported both the state and the reason, so the I/O wait is
      // measured. What it does not report is which app code caused it.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.confidence_basis =
          "the kernel reported this thread uninterruptible for " +
          ms(duration) + " and gave iowait=1 as the reason, so the I/O wait "
          "is measured rather than inferred from a stack that happened to "
          "look like a read";
      // The symptom is measured; which code did it is not established.
      issue.cause_status = model::CauseStatus::kUnknown;

      const std::string caller = payload_text(io, "kernel_caller");
      if (!caller.empty()) {
        issue.missing_evidence.push_back(
            "the app's own call stack. The kernel names where *it* blocked (" +
            caller +
            "), which is not the line of app code that asked for the read");
      } else {
        issue.missing_evidence.push_back(
            "any location at all: the kernel reported no caller and the app "
            "emitted no slice covering this block");
      }
      if (slice == nullptr) {
        issue.missing_evidence.push_back(
            "a userspace slice covering the block, which is what would say "
            "what the thread was doing; the app emitted none here");
      }
      issue.missing_evidence.push_back(
          "whether the user saw it: a blocked thread is not automatically a "
          "dropped frame, and this rule does not check the frame record");
      for (const auto& fp : known_false_positives()) {
        issue.alternative_explanations.push_back(fp);
      }
      issue.suggested_verification.push_back(
          "re-record the same flow on a warm device; a block that survives a "
          "warm page cache is doing real I/O, and one that does not was a "
          "cold-start cost");
      if (thread.is_js) {
        issue.proposed_remediation.push_back(
            "move the read off the JS thread: on this thread it blocks every "
            "queued JS task behind it, not only the current one");
      } else if (thread.is_main) {
        issue.proposed_remediation.push_back(
            "move the read off the UI thread, or make it asynchronous");
      } else if (thread.is_native_modules) {
        issue.proposed_remediation.push_back(
            "this is the queue JS calls into, so a block here delays every "
            "native module call behind it; move the read to its own "
            "background queue");
      }

      if (duration >= high) {
        issue.severity = model::Severity::kHigh;
      } else if (duration >= medium) {
        issue.severity = model::Severity::kMedium;
      } else {
        issue.severity = model::Severity::kLow;
      }
      issue.severity_rationale =
          "severity follows how long the thread was blocked, which is the "
          "impact ordering the spec asks for. It is not a claim that the user "
          "noticed";

      issue.threshold_expression = "uninterruptible with iowait >= " +
                                   ms(min_block) + " on a user-visible thread";
      issue.threshold_origin =
          "configurable_heuristic: no platform publishes an I/O budget, and "
          "the default sits near one frame at 120 Hz";

      model::Metric metric;
      metric.name = "io.blocked_duration_ns";
      metric.unit = "ns";
      metric.value = static_cast<double>(duration);
      metric.provider = "atrace";
      metric.method = model::MetricMethod::kMeasured;
      metric.aggregation = "single_uninterruptible_interval";
      metric.app_scoped = true;
      metric.window_start_ns = issue.start_ns;
      metric.window_end_ns = issue.end_ns;
      metric.process_instance_id = io.process_instance_id;
      metric.limitations.push_back(
          "the interval is the thread's whole uninterruptible stretch; the "
          "kernel reports the I/O reason for it but not how much of it was "
          "transfer versus waiting behind other I/O");
      issue.metrics.push_back(std::move(metric));

      model::EvidenceRef reason;
      reason.kind = "event";
      reason.id = io.event_id;
      reason.start_ns = io.timestamp;
      reason.note = caller.empty()
                        ? "kernel reported iowait with no caller"
                        : "kernel blocked in " + caller;
      reason.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(reason));

      model::EvidenceRef blocked;
      blocked.kind = "event";
      blocked.id = interval->event_id;
      blocked.start_ns = interval->timestamp;
      blocked.end_ns = issue.end_ns;
      blocked.note = "uninterruptible for " + ms(duration);
      blocked.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(blocked));

      if (slice != nullptr) {
        model::EvidenceRef context;
        context.kind = "event";
        context.id = slice->event_id;
        context.start_ns = slice->timestamp;
        context.end_ns = slice->timestamp + slice->duration_ns.value_or(0);
        context.note = "the app's own slice covering the block: " + slice->name;
        context.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(context));
      }

      issue.fingerprint = make_fingerprint(
          id(), version(), io.thread_instance_id,
          caller.empty() ? (slice != nullptr ? slice->name : io.event_id)
                         : caller);
      out.issues.push_back(std::move(issue));
    }
  }
};

}  // namespace

RulePtr make_det03_main_thread_io() { return std::make_shared<Det03>(); }

}  // namespace mpi::rules
