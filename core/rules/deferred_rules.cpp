// Detectors from the specification catalog that are not implemented yet.
//
// They are registered on purpose. Spec H05 requires an unsupported rule to be
// *reported as skipped*, and spec H11 requires "no findings" to be
// distinguishable from "no analysis". A rule the engine has never heard of
// can satisfy neither, so each one below declares its identity, its
// prerequisites, its delivery phase, and why it cannot run today.
#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

class DeferredRule final : public Rule {
 public:
  DeferredRule(std::string id_in, std::string category_in, std::string title_in,
               std::string phase, std::vector<Prerequisite> prereqs,
               std::string allowed_conclusion, std::string blocker)
      : id_(std::move(id_in)),
        category_(std::move(category_in)),
        title_(std::move(title_in)),
        phase_(std::move(phase)),
        prereqs_(std::move(prereqs)),
        allowed_conclusion_(std::move(allowed_conclusion)),
        blocker_(std::move(blocker)) {}

  std::string id() const override { return id_; }
  std::string version() const override { return "0"; }
  std::string category() const override { return category_; }
  std::string title() const override { return title_; }
  std::string delivery_phase() const override { return phase_; }
  std::vector<Prerequisite> prerequisites() const override { return prereqs_; }

  std::vector<std::string> unmet_prerequisites(const RuleContext&) const override {
    // Always skipped, with the reason and the phase stated.
    return {"not implemented: scheduled for " + phase_ + " -- " + blocker_,
            "allowed conclusion once implemented: " + allowed_conclusion_};
  }

  void evaluate(const RuleContext&, RuleOutput&) const override {
    // Unreachable: unmet_prerequisites() is never empty.
  }

 private:
  std::string id_;
  std::string category_;
  std::string title_;
  std::string phase_;
  std::vector<Prerequisite> prereqs_;
  std::string allowed_conclusion_;
  std::string blocker_;
};

}  // namespace

std::vector<RulePtr> make_deferred_rules() {
  std::vector<RulePtr> r;

  r.push_back(std::make_shared<DeferredRule>(
      "DET-03", "io", "Synchronous main-thread I/O", "M5",
      std::vector<Prerequisite>{
          {"io_events", "I/O events carrying the issuing thread"},
          {"stack_for_location", "a stack to locate the call site"}},
      "I/O observed on the main thread; severity depends on measured impact",
      "no validated I/O provider is wired up yet on either platform"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-05", "memory", "Memory growth across screen cycles", "M5",
      std::vector<Prerequisite>{
          {"lifecycle_checkpoints",
           "repeated comparable screen mount/unmount checkpoints"},
          {"memory_counters", "a memory counter series per checkpoint"}},
      "suspected retention -- never 'a leak' from growth alone, since caches "
      "and GC timing produce the same shape",
      "requires the SDK lifecycle markers from M3 plus a memory collector"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-06", "memory", "Retained-object investigation", "M5",
      std::vector<Prerequisite>{
          {"heap_snapshot", "a heap snapshot with reference paths"},
          {"lifecycle_expectation",
           "a stated expectation of when the object should have been freed"}},
      "supported retention, with its assumptions listed",
      "heap capture is not implemented; allocation volume is not retained size"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-07", "startup", "Startup budget exceedance", "M4",
      std::vector<Prerequisite>{
          {"startup_endpoints",
           "defined process-start, first-frame, and fully-usable endpoints"},
          {"configured_budget", "a project budget to compare against"},
          {"launch_class", "cold / warm / hot classification for the launch"}},
      "above budget for the stated launch class",
      "needs the benchmark scenario model and the app marker for 'fully "
      "usable'; a missing endpoint must produce an incomplete measurement, "
      "never a fast one"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-09", "scheduling", "Wait / lock contention", "M5",
      std::vector<Prerequisite>{
          {"scheduling_evidence",
           "running / runnable / blocked state transitions"},
          {"owner_proof",
           "proof of the lock owner before any owner is named"}},
      "wait observed; the cause stays qualified unless the owner is proven",
      "no scheduling provider is wired up; naming a lock owner without proof "
      "would be a fabricated cause"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-10", "react", "Repeated React renders", "M5",
      std::vector<Prerequisite>{
          {"react_profiling_data",
           "React profiler commit data -- Hermes CPU sampling is not a "
           "substitute"}},
      "a render pattern, which is not automatically a defect",
      "requires the React profiling integration from M3+; deriving renders "
      "from Hermes samples would misreport them"));

  r.push_back(std::make_shared<DeferredRule>(
      "DET-11", "network", "Network delay affecting an interaction", "M5",
      std::vector<Prerequisite>{
          {"request_spans", "request lifecycle spans"},
          {"interaction_spans", "the interaction they are claimed to delay"},
          {"dependency_evidence", "the async dependency between them"}},
      "delay observed; no server-side cause is assumed from client timing",
      "no validated network provider; request duration is not a server root "
      "cause"));

  return r;
}

}  // namespace mpi::rules
