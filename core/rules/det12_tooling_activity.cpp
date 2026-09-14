// DET-12 -- expensive development-tooling activity.
//
// This is the rule most easily abused, so its guardrails are the strictest in
// the set. It may only classify work whose ownership is *verified*: a library
// name is not proof that its work belongs to debugging (spec section 9 rule
// 4). Anything ambiguous stays shared_or_unknown. And the finding never
// carries a "release would be X faster" number, because subtracting overhead
// to estimate release performance is forbidden outright (rule 6, F14).
#include <algorithm>
#include <map>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

// Versioned ownership rules. Each entry states what it matches and what that
// match does and does not prove.
struct OwnershipRule {
  const char* rule_id;
  const char* version;
  const char* frame_substring;
  model::AttributionCategory category;
  const char* locus;
  const char* label;
  // True when matching this frame establishes ownership on its own. False
  // means the match is a hint that still leaves the slice unclassified.
  bool conclusive;
  const char* note;
};

// Deliberately short. Every entry here is something whose presence in a stack
// means the frame belongs to the tool, not to the product. Anything merely
// *associated* with development is marked non-conclusive.
constexpr OwnershipRule kOwnershipRules[] = {
    {"ATTR-PROFILER-SELF", "1", "mpi::", model::AttributionCategory::kProfiler,
     "device.collector", "mpi_collector",
     true, "frames inside this tool's own collector are ours by construction"},
    {"ATTR-RN-DEVSUPPORT", "1", "com.facebook.react.devsupport",
     model::AttributionCategory::kDevelopmentTooling, "device.app",
     "react_native_dev_support", true,
     "the React Native dev-support package exists only in development builds"},
    {"ATTR-RN-DEVSUPPORT-IOS", "1", "RCTDevSettings",
     model::AttributionCategory::kDevelopmentTooling, "device.app",
     "react_native_dev_settings", true,
     "RCTDevSettings is compiled out of release builds"},
    {"ATTR-RN-INSPECTOR", "1", "RCTInspector",
     model::AttributionCategory::kDevelopmentTooling, "device.app",
     "react_native_inspector", true,
     "the RN inspector runs only when dev tooling is attached"},
    {"ATTR-HERMES-SAMPLER", "1", "hermes::vm::SamplingProfiler",
     model::AttributionCategory::kProfiler, "device.app", "hermes_sampler",
     true, "the Hermes sampling profiler is the collector itself"},
    {"ATTR-METRO-HINT", "1", "metro",
     model::AttributionCategory::kSharedOrUnknown, "unknown", "metro_name_match",
     false,
     "a frame merely containing 'metro' proves nothing: the host dev server's "
     "CPU is not device CPU, and an app module may legitimately carry the name"},
    {"ATTR-FLIPPER-HINT", "1", "flipper",
     model::AttributionCategory::kSharedOrUnknown, "unknown", "flipper_name_match",
     false,
     "a name match alone does not establish that this work is debugging work"},
};

class Det12 final : public Rule {
 public:
  std::string id() const override { return "DET-12"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "tooling_overhead"; }
  std::string title() const override {
    return "Expensive development-tooling or profiler activity";
  }
  std::string delivery_phase() const override { return "M1"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"samples_or_tooling_events",
         "sampled stacks, or events a collector explicitly attributed to "
         "tooling"},
        {"verified_ownership",
         "at least one conclusive ownership rule matched; a library name alone "
         "is not sufficient"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"tooling_share", 0.10, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "share of a thread's samples above which the diagnostic "
                      "configuration is worth reporting as a measurement "
                      "problem"},
        ThresholdSpec{"high_share", 0.30, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "share above which the capture likely misrepresents the "
                      "app's own behavior"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "in-process dev tooling is genuinely part of the app process total in "
        "a debug build; reporting it is about measurement validity, not a "
        "product defect (spec F12)",
        "a shared allocator, GC, or scheduler effect caused by tooling cannot "
        "be cleanly separated and stays unclassified (section 9 rule 5)",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    const bool has_tooling_events =
        std::any_of(t.events.begin(), t.events.end(), [](const model::Event& e) {
          return e.category == model::EventCategory::kToolingActivity;
        });
    if (t.cpu_samples.empty() && !has_tooling_events) {
      unmet.push_back(
          "no sampled stacks and no collector-attributed tooling events; "
          "tooling cost cannot be attributed from nothing");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override;
};

const OwnershipRule* classify(const std::string& frame) {
  // Longest, most specific match wins; a conclusive rule beats a hint.
  const OwnershipRule* best = nullptr;
  for (const auto& r : kOwnershipRules) {
    if (frame.find(r.frame_substring) == std::string::npos) continue;
    if (!best) {
      best = &r;
      continue;
    }
    if (r.conclusive && !best->conclusive) {
      best = &r;
    } else if (r.conclusive == best->conclusive &&
               std::string(r.frame_substring).size() >
                   std::string(best->frame_substring).size()) {
      best = &r;
    }
  }
  return best;
}

void Det12::evaluate(const RuleContext& ctx, RuleOutput& out) const {
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
  const double tooling_share_threshold = value_of("tooling_share");
  const double high_share = value_of("high_share");

  struct Bucket {
    double total = 0.0;
    // Conclusively attributed weight, keyed by attribution label.
    std::map<std::string, double> attributed;
    std::map<std::string, const OwnershipRule*> rule_for_label;
    // Weight that matched only a hint and therefore stays unclassified.
    double hinted_unclassified = 0.0;
    std::map<std::string, model::TimeNs> first_seen;
    std::map<std::string, model::TimeNs> last_seen;
    std::string process_instance_id;
  };
  std::map<std::string, Bucket> buckets;

  for (const auto& s : t.cpu_samples) {
    if (ctx.cancel.cancelled()) return;
    if (s.frames.empty()) continue;
    Bucket& b = buckets[s.thread_instance_id];
    b.process_instance_id = s.process_instance_id;
    const double w = s.weight.value_or(1.0);
    b.total += w;

    // Scan the whole stack: tooling usually appears as a caller, not a leaf.
    const OwnershipRule* matched = nullptr;
    for (const auto& f : s.frames) {
      const OwnershipRule* r = classify(f);
      if (!r) continue;
      if (!matched || (r->conclusive && !matched->conclusive)) matched = r;
    }
    if (!matched) continue;
    if (!matched->conclusive) {
      b.hinted_unclassified += w;
      continue;
    }
    b.attributed[matched->label] += w;
    b.rule_for_label[matched->label] = matched;
    auto first = b.first_seen.find(matched->label);
    if (first == b.first_seen.end()) {
      b.first_seen[matched->label] = s.timestamp_ns;
    } else {
      first->second = std::min(first->second, s.timestamp_ns);
    }
    auto last = b.last_seen.find(matched->label);
    if (last == b.last_seen.end()) {
      b.last_seen[matched->label] = s.timestamp_ns;
    } else {
      last->second = std::max(last->second, s.timestamp_ns);
    }
  }

  for (const auto& kv : buckets) {
    const Bucket& b = kv.second;
    if (b.total <= 0.0) continue;
    // A thread whose only tooling-shaped frames were name-only matches still
    // gets an attribution report: "we saw something that looks like tooling
    // and could not attribute it" is exactly the kind of fact spec section 9
    // rule 5 requires to stay visible. Dropping the bucket here would hide it.
    if (b.attributed.empty() && b.hinted_unclassified <= 0.0) continue;

    double attributed_total = 0.0;
    for (const auto& a : b.attributed) attributed_total += a.second;
    const double share = attributed_total / b.total;

    // Build the attribution report regardless of whether the share crosses a
    // threshold: preserving the original total next to the attributed subset
    // is required by spec section 9 rule 9.
    model::AttributionReport report;
    report.original_total.name = "cpu.thread_sample_weight_total";
    report.original_total.unit = "count";
    report.original_total.value = b.total;
    report.original_total.provider = t.cpu_samples.front().provider;
    report.original_total.thread_instance_id = kv.first;
    report.original_total.process_instance_id = b.process_instance_id;
    report.original_total.window_start_ns = t.window_start_ns;
    report.original_total.window_end_ns = t.window_end_ns;
    report.original_total.method = model::MetricMethod::kSampled;
    report.original_total.aggregation = "sum";
    // Self-weight buckets over distinct labels do not overlap.
    report.slices_are_disjoint = true;
    report.limitations.push_back(
        "these slices describe where the collector saw tooling frames; they "
        "must not be subtracted from the total to estimate release "
        "performance");
    report.limitations.push_back(
        "shared allocator, GC, and scheduling effects caused by tooling are "
        "not separable and are not included in any slice");

    for (const auto& a : b.attributed) {
      const OwnershipRule* r = b.rule_for_label.at(a.first);
      model::AttributedSlice slice;
      slice.category = r->category;
      slice.basis = model::AttributionBasis::kMatchingStack;
      slice.label = a.first;
      slice.rule_id = r->rule_id;
      slice.rule_version = r->version;
      slice.value = a.second;
      slice.unit = "count";
      slice.locus = r->locus;
      slice.evidence_refs.push_back("cpu_samples:" + kv.first + ":" + a.first);
      report.slices.push_back(std::move(slice));
    }
    if (b.hinted_unclassified > 0.0) {
      model::AttributedSlice slice;
      slice.category = model::AttributionCategory::kSharedOrUnknown;
      slice.basis = model::AttributionBasis::kUnclassified;
      slice.label = "name_match_only_not_attributed";
      slice.rule_id = "ATTR-UNCLASSIFIED";
      slice.rule_version = "1";
      slice.value = b.hinted_unclassified;
      slice.unit = "count";
      slice.locus = "unknown";
      report.slices.push_back(std::move(slice));
      report.limitations.push_back(
          "samples whose stacks matched only a tooling *name* are reported as "
          "unclassified, not as tooling cost");
    }
    out.record.coverage_fraction = share;
    out.attribution.push_back(std::move(report));

    // The attribution above is always reported. An *issue*, though, requires
    // conclusively attributed work: an unclassified share is not evidence of a
    // tooling cost, only evidence that something could not be classified.
    if (b.attributed.empty()) continue;
    if (share < tooling_share_threshold) continue;

    const model::ThreadInfo* ti = t.thread(kv.first);
    model::Issue issue;
    issue.rule_id = id();
    issue.rule_version = version();
    issue.session_id = t.session_id;
    issue.category = category();
    issue.process_instance_id = b.process_instance_id;
    issue.thread_instance_id = kv.first;
    issue.mode = ctx.mode;
    issue.eligibility = ctx.eligibility;

    model::TimeNs start = t.window_end_ns;
    model::TimeNs end = t.window_start_ns;
    for (const auto& f : b.first_seen) start = std::min(start, f.second);
    for (const auto& l : b.last_seen) end = std::max(end, l.second);
    issue.start_ns = start;
    issue.end_ns = end;

    const int pct = static_cast<int>(share * 100.0);
    issue.title = "Development tooling accounts for " + std::to_string(pct) +
                  "% of sampled work on " +
                  (ti && !ti->name.empty() ? "thread '" + ti->name + "'"
                                           : "an unnamed thread");

    issue.detection_status = model::DetectionStatus::kObserved;
    // The cause of the *measurement problem* is established: we matched
    // conclusive ownership rules. That is a diagnostic-configuration finding.
    issue.cause_status = model::CauseStatus::kSupported;
    issue.confidence_basis =
        "every counted sample matched a conclusive ownership rule (a frame "
        "that exists only in development or profiler code); name-only matches "
        "were excluded and left unclassified";

    issue.severity = share >= high_share ? model::Severity::kHigh
                                         : model::Severity::kMedium;
    issue.severity_rationale =
        "severity reflects how much of the capture describes the tooling "
        "rather than the app; it is a measurement-validity concern, not a "
        "product defect";

    issue.threshold_expression =
        "conclusively attributed tooling share >= " +
        std::to_string(tooling_share_threshold) + " (high at " +
        std::to_string(high_share) + ")";
    issue.threshold_origin = "configurable_heuristic";

    model::Metric m;
    m.name = "attribution.development_tooling_sample_share";
    m.unit = "fraction";
    m.value = share;
    m.provider = t.cpu_samples.front().provider;
    m.thread_instance_id = kv.first;
    m.process_instance_id = b.process_instance_id;
    m.window_start_ns = t.window_start_ns;
    m.window_end_ns = t.window_end_ns;
    m.method = model::MetricMethod::kSampled;
    m.aggregation = "share_of_thread_samples";
    m.limitations.push_back(
        "in-process dev tooling is part of the observed app-process total; "
        "this share describes the capture, not a removable cost");
    issue.metrics.push_back(std::move(m));

    for (const auto& a : b.attributed) {
      model::EvidenceRef e;
      e.kind = "sample_set";
      e.id = "cpu_samples:" + kv.first + ":" + a.first;
      e.start_ns = b.first_seen.at(a.first);
      e.end_ns = b.last_seen.at(a.first);
      e.note = a.first + ": " + std::to_string(static_cast<long long>(a.second)) +
               " samples, rule " + b.rule_for_label.at(a.first)->rule_id;
      e.synthetic = t.synthetic;
      issue.evidence.push_back(std::move(e));
    }

    issue.missing_evidence.push_back(
        "a paired run with the same scenario and the tooling disabled, to "
        "measure the collector's actual overhead (spec section 9 rule 10)");
    issue.alternative_explanations.push_back(
        "some of the tooling's effect is shared (allocator, GC, scheduling) "
        "and is not captured by any slice");
    issue.suggested_verification.push_back(
        "record the identical scenario on an optimized build with dev tooling "
        "off, and compare the two runs as separate measurements");

    issue.proposed_remediation.push_back(
        "treat this capture as diagnostic only; for production-like numbers, "
        "re-record on an optimized build with development tooling disabled");
    issue.proposed_remediation.push_back(
        "do NOT subtract this share from the totals: the remainder is not an "
        "estimate of release performance");

    if (t.synthetic) issue.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);

    issue.occurrence_count = static_cast<std::int64_t>(attributed_total);
    issue.fingerprint =
        make_fingerprint(id(), version(), kv.first, "development_tooling_share");
    issue.issue_id = issue.fingerprint;
    out.issues.push_back(std::move(issue));
  }
}

}  // namespace

RulePtr make_det12_tooling_activity() { return std::make_shared<Det12>(); }

}  // namespace mpi::rules
