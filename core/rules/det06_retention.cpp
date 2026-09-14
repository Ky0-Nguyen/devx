// DET-06 -- retained-object investigation.
//
// The strongest thing the specification lets this rule say is "supported
// retention, with its assumptions listed" (section 10.2), and a heap dump is
// exactly the kind of evidence that invites saying more. So the boundary is
// worth stating precisely.
//
// What a dump proves:
//   * that an object existed at one instant, and
//   * what chain of references reached it from a root.
// Both are read from the file. Neither is inferred.
//
// What it cannot prove:
//   * that anything leaked. Reachable is not leaked -- a cache, a singleton,
//     an object pool and a framework-held instance are all reachable by
//     design. "Leak" appears nowhere in this rule's output.
//   * that the object would have survived a collection. `am dumpheap`
//     triggers one first, but a finalizer queue or a phantom reference can
//     keep an object one cycle longer, and one dump cannot tell that from
//     retention.
//   * how much memory it holds. That is retained size, which needs a
//     dominator tree; the rule reports shallow size and says which it is.
//
// So the rule reports what it can check: an object whose own lifecycle state
// says it is finished, still held by a chain of references. The platform
// documents that a destroyed Activity should be released, which makes
// `mDestroyed` a defensible expectation rather than this tool's opinion --
// and the finding says which field it read, because a framework's private
// field is a version-dependent thing to depend on.
#include <algorithm>
#include <map>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

// A class whose instances the platform expects to be released, and the field
// that says so. Nothing here is this tool's judgement about the app: each row
// is a documented framework lifecycle with a state the object publishes about
// itself.
struct LifecycleExpectation {
  const char* class_name;
  const char* finished_flag;
  const char* expectation;
};

const LifecycleExpectation kExpectations[] = {
    {"android.app.Activity", "mDestroyed",
     "an Activity that has been destroyed is expected to be released: the "
     "framework has finished with it and will not use it again"},
    {"android.app.Activity", "mFinished",
     "an Activity that has been finished is on its way out and is expected "
     "to be released once its teardown completes"},
    {"androidx.fragment.app.Fragment", "mRemoving",
     "a Fragment being removed from its manager is expected to be released "
     "once the transaction completes"},
};

struct Candidate {
  std::string class_name;
  std::string flag_name;
  std::string expectation;
  std::vector<heap::ObjectId> objects;
  std::vector<std::string> matched_subclasses;
};

std::string describe_path(const heap::ReferencePath& p) {
  std::string s = p.root_class.empty() ? std::string("a GC root")
                                       : p.root_class;
  s += " (held as a " + std::string(heap::to_string(p.root.kind)) + " root)";
  for (const auto& step : p.steps) {
    s += "\n    -> ";
    if (step.via_index.has_value()) {
      s += "[" + std::to_string(*step.via_index) + "] of " + step.holder_class;
    } else {
      s += step.holder_class + "." + step.via_field;
    }
  }
  s += "\n    -> " + p.target_class;
  return s;
}

class Det06 final : public Rule {
 public:
  std::string id() const override { return "DET-06"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "memory"; }
  std::string title() const override { return "Retained-object investigation"; }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"heap_snapshot",
         "a heap dump with reference paths, from `mpi record --heap` on "
         "Android; memory counters cannot substitute, because a counter says "
         "how much is held and never by what"},
        {"lifecycle_expectation",
         "a stated expectation of when the object should have been freed. "
         "Without one, a reachable object is just a reachable object"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_instances", 1, "count",
                      ThresholdOrigin::kPlatformContract,
                      "one destroyed-but-held instance is already the thing "
                      "the platform says should not be there, so the default "
                      "does not wait for a pattern"},
        ThresholdSpec{"max_path_hops", 40, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a bound on the search, not a claim: a longer chain is "
                      "not reported rather than being reported as absent"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "the framework itself keeps the last destroyed Activity for a "
        "configuration change or for its recents entry, which is by design",
        "the dump was taken while the object was on a finalizer or reference "
        "queue, where it is legitimately held for one more collection cycle",
        "a debug build's tooling -- LeakCanary, an instrumentation hook, a "
        "profiler -- holds a reference precisely to watch it",
        "a deliberate cache keyed on the object, where retention is the "
        "feature and not the fault",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    if (ctx.heap_graph == nullptr) {
      unmet.push_back(
          "no heap dump was collected. Reference paths come from a heap "
          "snapshot (`mpi record --heap` on Android, which runs `am "
          "dumpheap`); the memory counters this capture may contain say how "
          "much memory was held and never by what, so they cannot stand in");
      return unmet;
    }
    const auto& g = *ctx.heap_graph;
    if (g.objects().empty()) {
      unmet.push_back(
          "the heap dump contained no objects, so there is no graph to search");
      return unmet;
    }
    if (candidates(g).empty()) {
      // Present the absence precisely: the dump was read, the classes were
      // looked for, and none of them published a finished state.
      unmet.push_back(
          "this dump contains no object with a lifecycle state this build "
          "knows how to check. It looks for a destroyed or finished Activity "
          "(`mDestroyed`, `mFinished`) and a removing Fragment "
          "(`mRemoving`); an object with no published expectation cannot be "
          "judged retained, because a reachable object is a normal object");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    if (ctx.heap_graph == nullptr) return;
    const auto& g = *ctx.heap_graph;
    const auto min_instances =
        ctx.override_for("DET-06.min_instances").value_or(1.0);
    const auto max_hops =
        static_cast<std::size_t>(ctx.override_for("DET-06.max_path_hops")
                                     .value_or(40.0));

    if (!g.gc_requested_before_dump) {
      // Stated once, on the run record, because it qualifies everything
      // below: without a collection first, an unreachable object in the dump
      // is simply unswept.
      out.record.skipped_reasons.push_back(
          "the dump was taken without a collection being requested first, so "
          "an object still present here may simply not have been swept yet");
    }

    for (const auto& cand : candidates(g)) {
      if (ctx.cancel.cancelled()) return;
      if (static_cast<double>(cand.objects.size()) < min_instances) {
        out.record.skipped_reasons.push_back(
            cand.class_name + ": " + std::to_string(cand.objects.size()) +
            " instance(s) report " + cand.flag_name +
            ", below min_instances");
        continue;
      }

      // A path per instance, up to a few: the chains are usually the same
      // shape, and one finding with a worked example is more use than one
      // finding per object.
      std::vector<heap::ReferencePath> paths;
      std::size_t unreachable = 0;
      std::size_t app_rooted = 0;
      for (const auto id : cand.objects) {
        if (ctx.cancel.cancelled()) return;
        auto p = g.path_from_root(id);
        if (!p.found) {
          ++unreachable;
          continue;
        }
        if (p.steps.size() > max_hops) continue;
        if (heap::root_is_app_meaningful(p.root.kind)) ++app_rooted;
        if (paths.size() < 3) paths.push_back(std::move(p));
      }

      if (paths.empty()) {
        // Every instance was unreachable. That is the opposite of retention:
        // the objects are garbage awaiting collection, and saying so is the
        // finding.
        out.record.skipped_reasons.push_back(
            cand.class_name + ": " + std::to_string(cand.objects.size()) +
            " instance(s) report " + cand.flag_name +
            " and no root reaches any of them, so they are garbage awaiting "
            "collection rather than objects being held");
        continue;
      }

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = ctx.trace->session_id;
      issue.category = category();
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      // A heap dump is one instant, and that instant is the dump's own
      // timestamp -- not a position in the capture window. Leaving the
      // interval at the window start would put it somewhere it was not.
      issue.start_ns = ctx.trace->window_start_ns;
      issue.end_ns = ctx.trace->window_start_ns;

      const auto held = paths.size();
      issue.title =
          std::to_string(cand.objects.size()) + " instance(s) of " +
          cand.class_name + " report " + cand.flag_name +
          " and are still reachable";

      // The path is measured: it was read from the dump. Whether retention is
      // a fault is not measured at all.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.cause_status = model::CauseStatus::kCandidate;
      issue.confidence_basis =
          "the reference chain is read from the heap dump, so what holds the "
          "object is measured rather than inferred. Whether holding it is a "
          "fault is not: " + cand.expectation +
          ", but the framework, a cache, or a debug tool may hold it "
          "deliberately, and one dump cannot tell those apart";

      issue.severity = model::Severity::kMedium;
      issue.severity_rationale =
          "capped at medium: a reference chain establishes what holds an "
          "object, not that holding it costs the user anything. Nothing here "
          "measures memory pressure, a dropped frame, or a termination, so "
          "this must not outrank a finding that does";

      issue.threshold_expression =
          "instances reporting " + cand.flag_name + " >= " +
          std::to_string(static_cast<int>(min_instances));
      issue.threshold_origin =
          "platform_contract: the expectation comes from the framework's "
          "own lifecycle, read from the object's own `" + cand.flag_name +
          "` field, not from a number this tool chose";

      for (std::size_t i = 0; i < paths.size(); ++i) {
        model::CandidateStack stack;
        stack.note = "reference chain " + std::to_string(i + 1) + " of " +
                     std::to_string(held) + " shown:\n    " +
                     describe_path(paths[i]);
        issue.candidate_stacks.push_back(std::move(stack));

        model::EvidenceRef ref;
        ref.kind = "heap_reference_path";
        ref.id = "object-" + std::to_string(paths[i].target);
        ref.note = "held via " + std::to_string(paths[i].steps.size()) +
                   " reference(s) from a " +
                   heap::to_string(paths[i].root.kind) + " root";
        ref.synthetic = ctx.trace->synthetic;
        issue.evidence.push_back(std::move(ref));
      }

      model::Metric count;
      count.name = "heap.instances_past_lifecycle";
      count.unit = "count";
      count.value = static_cast<double>(cand.objects.size());
      count.provider = g.provider;
      count.method = model::MetricMethod::kMeasured;
      count.aggregation = "count_of_instances_reporting_" + cand.flag_name;
      count.app_scoped = true;
      count.limitations.push_back(
          "counted in one dump: this is how many existed at that instant, "
          "not how many were created or how long any of them lived");
      issue.metrics.push_back(std::move(count));

      // Shallow size, named as shallow. The number people want is retained
      // size, and giving them shallow size under that name would be the
      // single most misleading thing this rule could do.
      std::int64_t shallow = 0;
      for (const auto id2 : cand.objects) {
        if (const heap::Object* o = g.find_object(id2)) shallow += o->shallow_size;
      }
      model::Metric bytes;
      bytes.name = "heap.shallow_bytes_of_those_instances";
      bytes.unit = "bytes";
      bytes.value = static_cast<double>(shallow);
      bytes.provider = g.provider;
      bytes.method = model::MetricMethod::kMeasured;
      bytes.aggregation = "sum_of_shallow_sizes";
      bytes.app_scoped = true;
      bytes.limitations.push_back(
          "shallow, not retained: this is the objects' own bytes and excludes "
          "everything they reference. An Activity holding a 40 MB bitmap has "
          "a shallow size of a few hundred bytes, so this number is a floor "
          "and usually a very low one");
      bytes.limitations.push_back(
          "retained size is not computed: it needs a dominator tree over the "
          "whole graph, and an approximation presented as a size would be a "
          "number nobody could check");
      issue.metrics.push_back(std::move(bytes));

      issue.missing_evidence.push_back(
          "retained size: what these objects actually hold, which is what "
          "decides whether this costs memory at all");
      issue.missing_evidence.push_back(
          "a second dump after another cycle of the same screen. One dump "
          "cannot distinguish a single instance the framework keeps by design "
          "from a count that grows with every visit");
      if (!g.gc_requested_before_dump) {
        issue.missing_evidence.push_back(
            "a collection before the dump: without one, an object here may "
            "simply not have been swept yet");
      }
      if (app_rooted == 0) {
        issue.missing_evidence.push_back(
            "a root in the app's own code. Every chain found is anchored in "
            "runtime bookkeeping -- an interned string, a VM internal -- "
            "which says the runtime holds these objects and not that the app "
            "does");
      }
      if (unreachable > 0) {
        issue.missing_evidence.push_back(
            std::to_string(unreachable) + " of these instance(s) had no root "
            "at all and are garbage awaiting collection; they are counted in "
            "the instance total but no chain is shown for them");
      }
      const auto& lim = g.limits;
      if (lim.dangling_references > 0 || lim.truncated_by_object_cap ||
          lim.truncated_by_byte_cap) {
        issue.missing_evidence.push_back(
            "the graph is incomplete -- " +
            std::to_string(lim.dangling_references) +
            " reference(s) point at objects not in it -- so a chain shown "
            "here may not be the shortest one that exists");
      }
      if (!cand.matched_subclasses.empty()) {
        std::string s = "counted as " + cand.class_name + ": ";
        for (std::size_t i = 0; i < cand.matched_subclasses.size() && i < 6; ++i) {
          if (i != 0) s += ", ";
          s += cand.matched_subclasses[i];
        }
        issue.suggested_verification.push_back(s);
      }

      for (const auto& fp : known_false_positives()) {
        issue.alternative_explanations.push_back(fp);
      }
      issue.alternative_explanations.push_back(
          "the field this rule read (`" + cand.flag_name +
          "`) is framework-private and its meaning can change between "
          "platform versions; on a version where it means something else, "
          "this finding means something else too");

      issue.suggested_verification.push_back(
          "follow the chain above to the last reference the app's own code "
          "owns, then re-record the same screen cycle twice and compare the "
          "instance count: a count that grows per visit is the thing worth "
          "fixing, and a count that stays at one usually is not");
      issue.proposed_remediation.push_back(
          "clear the reference named in the chain when the screen is torn "
          "down, if the app owns it. If the holder is a framework or library "
          "object, check whether it is documented to hold this and for how "
          "long before treating it as a defect");

      issue.fingerprint =
          make_fingerprint(id(), version(), cand.class_name, cand.flag_name);
      out.issues.push_back(std::move(issue));
    }
  }

 private:
  // The objects whose own lifecycle state says they are finished.
  static std::vector<Candidate> candidates(const heap::HeapGraph& g) {
    std::vector<Candidate> out;
    for (const auto& exp : kExpectations) {
      std::vector<std::string> matched;
      const auto ids = g.instances_assignable_to(exp.class_name, &matched);
      Candidate c;
      for (const auto id : ids) {
        const heap::Object* o = g.find_object(id);
        if (o == nullptr) continue;
        const auto flag = o->flag(exp.finished_flag);
        // Absent is not false. An object without the field is one this build
        // cannot judge, and it is left out rather than counted either way.
        if (!flag.has_value() || !*flag) continue;
        c.objects.push_back(id);
      }
      if (c.objects.empty()) continue;
      c.class_name = exp.class_name;
      c.flag_name = exp.finished_flag;
      c.expectation = exp.expectation;
      c.matched_subclasses = std::move(matched);
      out.push_back(std::move(c));
    }
    return out;
  }
};

}  // namespace

RulePtr make_det06_retention() { return std::make_shared<Det06>(); }

}  // namespace mpi::rules
