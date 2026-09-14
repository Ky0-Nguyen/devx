// DET-04 -- CPU hotspot from sampled stacks.
//
// Samples estimate attribution. They are not exact function durations, a
// missing sample is not idle time, and an inclusive share cannot be summed
// with its callees as a disjoint cost. This rule therefore reports a *sampled
// hotspot* and refuses to name a function at all unless symbols resolved
// (spec DET-04 minimum evidence, E13, E14, F17).
#include <algorithm>
#include <cmath>
#include <map>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

class Det04 final : public Rule {
 public:
  std::string id() const override { return "DET-04"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "cpu"; }
  std::string title() const override { return "Sampled CPU hotspot"; }
  std::string delivery_phase() const override { return "M1"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"cpu_samples", "sampled stacks for the target process"},
        {"min_sample_population",
         "enough samples that a share is meaningful rather than noise"},
        {"symbols_when_naming",
         "resolved symbols before any function name is attributed; without "
         "them the hotspot is reported by raw frame only"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_samples", 100, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "below this population a share is not distinguishable "
                      "from sampling noise"},
        ThresholdSpec{"hotspot_share", 0.15, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "an initial default for a leaf worth investigating; "
                      "there is no universal 'CPU > 80% is a bug' rule"},
        ThresholdSpec{"high_share", 0.40, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "an initial default for a dominant leaf"},
        ThresholdSpec{"max_hotspots", 5, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "caps how many hotspots one capture reports"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "high CPU without any user-visible harm is not a defect (spec E05)",
        "a sampler misses functions shorter than its interval, so an absent "
        "leaf is not proof of absent work (E14)",
        "an optimized, inlined, or tail-called frame may be attributed to its "
        "caller (E15)",
        "background computation that does not affect the UI can dominate the "
        "sample population (E04)",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    if (t.cpu_samples.empty()) {
      unmet.push_back(
          "no CPU samples were collected; a missing sample is not idle time, so "
          "no conclusion about CPU is available");
      return unmet;
    }
    const auto min_samples = static_cast<std::size_t>(
        ctx.override_for("DET-04.min_samples").value_or(100.0));
    if (t.cpu_samples.size() < min_samples) {
      unmet.push_back("only " + std::to_string(t.cpu_samples.size()) +
                      " sample(s) were collected, below the min_samples "
                      "threshold of " + std::to_string(min_samples) +
                      "; a share computed from this few samples would be noise");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override;
};

void Det04::evaluate(const RuleContext& ctx, RuleOutput& out) const {
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
  const double hotspot_share = value_of("hotspot_share");
  const double high_share = value_of("high_share");
  const auto max_hotspots = static_cast<std::size_t>(value_of("max_hotspots"));

  // Per-thread, because a hotspot on a background thread and one on the UI
  // thread are different findings.
  struct ThreadBucket {
    std::string thread_instance_id;
    std::string process_instance_id;
    double total_weight = 0.0;
    // Leaf frame -> self weight. Self, not inclusive: only self weights can be
    // compared against each other as disjoint costs.
    std::map<std::string, double> self_weight;
    // Leaf frame -> a representative full stack, for the evidence panel.
    std::map<std::string, std::vector<std::string>> example_stack;
    std::map<std::string, model::TimeNs> first_seen;
    std::map<std::string, model::TimeNs> last_seen;
    std::map<std::string, std::int64_t> sample_count;
  };
  std::map<std::string, ThreadBucket> buckets;

  for (const auto& s : t.cpu_samples) {
    if (ctx.cancel.cancelled()) return;
    if (s.frames.empty()) continue;
    ThreadBucket& b = buckets[s.thread_instance_id];
    b.thread_instance_id = s.thread_instance_id;
    b.process_instance_id = s.process_instance_id;
    const double w = s.weight.value_or(1.0);
    b.total_weight += w;
    const std::string& leaf = s.frames.back();
    b.self_weight[leaf] += w;
    if (b.example_stack.find(leaf) == b.example_stack.end()) {
      b.example_stack[leaf] = s.frames;
    }
    auto first = b.first_seen.find(leaf);
    if (first == b.first_seen.end()) {
      b.first_seen[leaf] = s.timestamp_ns;
    } else {
      first->second = std::min(first->second, s.timestamp_ns);
    }
    auto last = b.last_seen.find(leaf);
    if (last == b.last_seen.end()) {
      b.last_seen[leaf] = s.timestamp_ns;
    } else {
      last->second = std::max(last->second, s.timestamp_ns);
    }
    ++b.sample_count[leaf];
  }

  // Sampling interval, needed to say how much of the window a sample stands
  // for. Derived from the observed median gap, not assumed.
  std::optional<double> median_interval_ns;
  {
    std::vector<model::TimeNs> gaps;
    for (std::size_t i = 1; i < t.cpu_samples.size(); ++i) {
      const model::TimeNs d =
          t.cpu_samples[i].timestamp_ns - t.cpu_samples[i - 1].timestamp_ns;
      if (d > 0) gaps.push_back(d);
    }
    if (!gaps.empty()) {
      std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2),
                       gaps.end());
      median_interval_ns = static_cast<double>(gaps[gaps.size() / 2]);
    }
  }

  std::size_t emitted = 0;
  for (const auto& kv : buckets) {
    const ThreadBucket& b = kv.second;
    if (b.total_weight <= 0.0) continue;

    std::vector<std::pair<std::string, double>> ranked(b.self_weight.begin(),
                                                       b.self_weight.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& c) { return a.second > c.second; });

    const model::ThreadInfo* ti = t.thread(b.thread_instance_id);

    for (const auto& entry : ranked) {
      if (emitted >= max_hotspots) break;
      const double share = entry.second / b.total_weight;
      if (share < hotspot_share) break;  // ranked, so nothing below qualifies

      model::Issue issue;
      issue.rule_id = id();
      issue.rule_version = version();
      issue.session_id = t.session_id;
      issue.category = category();
      issue.process_instance_id = b.process_instance_id;
      issue.thread_instance_id = b.thread_instance_id;
      issue.mode = ctx.mode;
      issue.eligibility = ctx.eligibility;
      issue.start_ns = b.first_seen.at(entry.first);
      issue.end_ns = b.last_seen.at(entry.first);

      // Resolve symbols before deciding how to name the finding.
      model::SourceLocation loc;
      if (ctx.symbol_service) loc = ctx.symbol_service->resolve_frame(entry.first);
      issue.symbol_status = loc.symbol_status;
      const bool named = loc.symbol_status == "exact_build_match" ||
                         loc.symbol_status == "partial";

      const int pct = static_cast<int>(std::lround(share * 100.0));
      const std::string thread_label =
          ti && !ti->name.empty() ? "thread '" + ti->name + "'" : "an unnamed thread";
      if (named) {
        issue.title = "Sampled CPU hotspot: " +
                      (loc.symbol.empty() ? entry.first : loc.symbol) + " holds " +
                      std::to_string(pct) + "% of samples on " + thread_label;
      } else {
        // Spec DET-04: symbols are required *before naming functions*.
        issue.title = "Sampled CPU hotspot: one unresolved frame holds " +
                      std::to_string(pct) + "% of samples on " + thread_label;
        issue.missing_evidence.push_back(
            "symbols for the hot frame; without them the raw provider string "
            "'" + entry.first + "' is not a function name");
      }

      // A sampled share is an observation about the sample population.
      issue.detection_status = model::DetectionStatus::kObserved;
      issue.cause_status = model::CauseStatus::kUnknown;
      issue.confidence_basis =
          "share of " + std::to_string(b.sample_count.at(entry.first)) + " self "
          "samples out of " + std::to_string(static_cast<long long>(b.total_weight)) +
          " on this thread; a sampled share estimates where time went, it is "
          "not a measured function duration";

      issue.alternative_explanations.push_back(
          "the sampler may attribute inlined or tail-called work to this frame");
      issue.alternative_explanations.push_back(
          "high CPU on this thread does not by itself establish user-visible "
          "harm");
      if (ti && !ti->is_main_ui_thread) {
        issue.alternative_explanations.push_back(
            "this is not the main UI thread, so the work may be intentionally "
            "backgrounded");
      }
      issue.missing_evidence.push_back(
          "evidence linking this CPU time to a user-visible symptom (frames, "
          "interaction latency, or a startup endpoint)");
      issue.suggested_verification.push_back(
          "re-record the same scenario and confirm the share is reproducible, "
          "then check whether a frame or interaction metric moves with it");

      if (share >= high_share && ti && ti->is_main_ui_thread) {
        issue.severity = model::Severity::kHigh;
      } else if (share >= high_share) {
        issue.severity = model::Severity::kMedium;
      } else {
        issue.severity = model::Severity::kLow;
      }
      issue.severity_rationale =
          "severity is ordered by sampled share and whether the thread is the "
          "main UI thread; it carries no claim about the cause";

      issue.threshold_expression = "self sample share >= " +
                                   std::to_string(hotspot_share) + " (high at " +
                                   std::to_string(high_share) + ")";
      issue.threshold_origin =
          "configurable_heuristic; deliberately not a 'CPU > 80% is a bug' rule";

      model::Metric share_m;
      share_m.name = "cpu.sampled_self_share";
      share_m.unit = "fraction";
      share_m.value = share;
      share_m.provider = t.cpu_samples.front().provider;
      share_m.process_instance_id = b.process_instance_id;
      share_m.thread_instance_id = b.thread_instance_id;
      share_m.window_start_ns = t.window_start_ns;
      share_m.window_end_ns = t.window_end_ns;
      share_m.method = model::MetricMethod::kSampled;
      share_m.aggregation = "self_share_of_thread_samples";
      // Declaring the normalization is mandatory for any CPU percentage.
      share_m.cpu_normalization = model::CpuNormalization::kNotApplicable;
      share_m.limitations.push_back(
          "share of this thread's samples, not a share of a core or of wall "
          "time");
      share_m.limitations.push_back(
          "self time only; it must not be added to an inclusive share of the "
          "same stack");
      if (median_interval_ns.has_value()) {
        share_m.limitations.push_back(
            "observed median sampling interval " +
            std::to_string(static_cast<long long>(*median_interval_ns / 1e6)) +
            " ms: work shorter than this can be missed entirely");
      }
      if (const model::Coverage* cov = t.coverage_for(share_m.provider)) {
        share_m.coverage_fraction = cov->covered_fraction();
        if (cov->covered_fraction() < 0.95) {
          share_m.limitations.push_back(
              "the sampler covered only part of the window; uncovered "
              "intervals are gaps, not idle time");
          issue.missing_evidence.push_back(
              "sampler coverage for the uncovered part of the capture window");
        }
      }
      issue.metrics.push_back(std::move(share_m));

      model::Metric count_m;
      count_m.name = "cpu.self_sample_count";
      count_m.unit = "count";
      count_m.value = static_cast<double>(b.sample_count.at(entry.first));
      count_m.provider = t.cpu_samples.front().provider;
      count_m.thread_instance_id = b.thread_instance_id;
      count_m.window_start_ns = t.window_start_ns;
      count_m.window_end_ns = t.window_end_ns;
      count_m.method = model::MetricMethod::kMeasured;
      count_m.aggregation = "sum";
      issue.metrics.push_back(std::move(count_m));

      model::CandidateStack stack;
      stack.frames = b.example_stack.at(entry.first);
      stack.sample_share = share;
      // This share is a self share of the leaf, so it is disjoint-comparable.
      stack.inclusive = false;
      stack.note =
          "a representative stack observed at this leaf; other stacks may also "
          "reach it";
      if (ctx.symbol_service) {
        for (const auto& frame : stack.frames) {
          stack.locations.push_back(ctx.symbol_service->resolve_frame(frame));
        }
      }
      issue.candidate_stacks.push_back(std::move(stack));

      model::EvidenceRef e;
      e.kind = "sample_set";
      e.id = "cpu_samples:" + b.thread_instance_id + ":" + entry.first;
      e.start_ns = issue.start_ns;
      e.end_ns = issue.end_ns;
      e.note = std::to_string(b.sample_count.at(entry.first)) +
               " self samples at this leaf";
      e.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(e));

      if (t.synthetic) issue.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);

      issue.proposed_remediation.push_back(
          "profile this stack with a finer sampling interval or targeted "
          "instrumentation to convert the sampled estimate into a measured "
          "duration before optimizing");

      issue.occurrence_count = b.sample_count.at(entry.first);
      issue.fingerprint =
          make_fingerprint(id(), version(), b.thread_instance_id, entry.first);
      issue.issue_id = issue.fingerprint;
      out.issues.push_back(std::move(issue));
      ++emitted;
    }
  }
}

}  // namespace

RulePtr make_det04_cpu_hotspot() { return std::make_shared<Det04>(); }

}  // namespace mpi::rules
