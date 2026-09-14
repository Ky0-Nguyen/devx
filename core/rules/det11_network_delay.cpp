// DET-11 -- network delay affecting an interaction.
//
// The allowed conclusion is "Delay observed, no assumed server cause"
// (section 10.2), and the second half is the hard part. When an interaction
// waits on a request, the elapsed time is measured by the app -- but *where*
// it went is not. A slow request is equally consistent with DNS, a TLS
// handshake, a cold radio, a retry, a proxy, client-side queueing behind other
// requests, the app's own main thread not reading the response, and a slow
// server. This rule names all of them and blames none.
//
// It also requires the overlap to be real. A request that merely happened
// during an interaction is not a request the interaction waited on, so the
// finding is only made when the request covers a meaningful share of the
// interaction's own span.
#include <algorithm>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

struct Span {
  model::TimeNs start_ns = 0;
  model::TimeNs end_ns = 0;
  std::string label;
  const model::Marker* marker = nullptr;
  std::string clock_domain;
};

std::string ms(model::TimeNs ns) { return std::to_string(ns / 1000000) + " ms"; }

// Interaction spans, from begin/end pairs. An interaction that never ended is
// not a span: its duration is unknown, not zero.
std::vector<Span> interaction_spans(const model::NormalizedTrace& t) {
  std::vector<Span> spans;
  std::vector<Span> open;
  for (const auto& m : t.markers) {
    if (m.kind == "interaction_begin") {
      open.push_back(Span{m.timestamp_ns, 0, m.interaction, &m, m.clock_domain});
      continue;
    }
    if (m.kind != "interaction_end") continue;
    // An end with its own duration closes the pair even if the begin was
    // lost, which happens when a batch went missing.
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
      if (it->label != m.interaction || it->end_ns != 0) continue;
      it->end_ns = m.timestamp_ns;
      spans.push_back(*it);
      break;
    }
  }
  return spans;
}

class Det11 final : public Rule {
 public:
  std::string id() const override { return "DET-11"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "network"; }
  std::string title() const override {
    return "Network delay affecting an interaction";
  }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"request_spans",
         "network request markers with durations, reported by the app"},
        {"interaction_spans",
         "an interaction with a begin and an end, so there is something the "
         "request could have delayed"},
        {"overlap",
         "the request must overlap the interaction; happening nearby is not "
         "the same as being waited on"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_request_ms", 300, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only; a request faster than this is "
                      "rarely what a user notices"},
        ThresholdSpec{"min_share_of_interaction", 0.50, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "the request must cover this much of the interaction "
                      "before the interaction is said to have waited on it"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a request the interaction did not actually wait on: overlapping in "
        "time is not the same as blocking, and prefetching looks identical "
        "from here",
        "an interaction that was already complete for the user while a "
        "request finished in the background",
        "a request that was slow because the app queued it behind others, "
        "which is the app's scheduling and not the network",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    const bool any_request = std::any_of(
        t.markers.begin(), t.markers.end(), [](const model::Marker& m) {
          return m.kind == "network_request" && m.duration_ns.has_value();
        });
    if (!any_request) {
      unmet.push_back(
          "no network request spans were collected. These come from the app's "
          "own request layer through the SDK (`networkRequest`); this tool "
          "does not observe the network, and inferring requests from CPU "
          "samples or traffic counters would be guesswork");
    }
    if (interaction_spans(t).empty()) {
      unmet.push_back(
          "no completed interaction span was recorded, so there is nothing a "
          "request could be said to have delayed");
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const auto min_request = static_cast<model::TimeNs>(
        ctx.override_for("DET-11.min_request_ms").value_or(300.0) * 1000000.0);
    const double min_share =
        ctx.override_for("DET-11.min_share_of_interaction").value_or(0.50);

    const auto interactions = interaction_spans(t);

    for (const auto& m : t.markers) {
      if (ctx.cancel.cancelled()) return;
      if (m.kind != "network_request" || !m.duration_ns.has_value()) continue;
      const model::TimeNs duration = *m.duration_ns;
      if (duration < min_request) continue;

      // The request's span, taken as ending at its marker: the app reports a
      // completed request, so the marker is the end and the duration reaches
      // back from it.
      const model::TimeNs request_end = m.timestamp_ns;
      const model::TimeNs request_start = request_end - duration;

      for (const auto& interaction : interactions) {
        const model::TimeNs overlap_start =
            std::max(request_start, interaction.start_ns);
        const model::TimeNs overlap_end =
            std::min(request_end, interaction.end_ns);
        if (overlap_end <= overlap_start) continue;
        const model::TimeNs interaction_span =
            interaction.end_ns - interaction.start_ns;
        if (interaction_span <= 0) continue;
        const double share = static_cast<double>(overlap_end - overlap_start) /
                             static_cast<double>(interaction_span);
        if (share < min_share) {
          out.record.skipped_reasons.push_back(
              "a " + ms(duration) + " request overlapped interaction '" +
              interaction.label + "' for only " +
              std::to_string(static_cast<int>(share * 100)) +
              "% of it, which is not enough to say the interaction waited on "
              "it");
          continue;
        }

        const std::string url =
            m.payload.find("url") != nullptr ? m.payload.find("url")->as_string()
                                             : std::string();
        const std::string method =
            m.payload.find("method") != nullptr
                ? m.payload.find("method")->as_string()
                : std::string();

        model::Issue issue;
        issue.rule_id = id();
        issue.rule_version = version();
        issue.session_id = t.session_id;
        issue.category = category();
        issue.mode = ctx.mode;
        issue.eligibility = ctx.eligibility;
        issue.screen = m.screen.empty() ? interaction.marker->screen : m.screen;
        issue.interaction = interaction.label;
        // Both spans are the app's, so comparing them is valid on the app's
        // clock -- but the reported interval is placed on the capture's
        // timeline so the UI focuses the right stretch.
        const auto interval =
            map_producer_interval(t, m.clock_domain, overlap_start, overlap_end);
        issue.start_ns = interval.start_ns;
        issue.end_ns = interval.end_ns;

        issue.title = "interaction '" + interaction.label + "' overlapped a " +
                      ms(duration) + " request for " +
                      std::to_string(static_cast<int>(share * 100)) +
                      "% of its span";

        // The timing is measured by the app. That the request *caused* the
        // interaction to feel slow is a temporal correlation, which the spec
        // caps at a candidate cause.
        issue.detection_status = model::DetectionStatus::kObserved;
        issue.confidence_basis =
            "both spans are the app's own measurements: the request took " +
            ms(duration) + " and covered " +
            std::to_string(static_cast<int>(share * 100)) +
            "% of an interaction that lasted " + ms(interaction_span) +
            ". That the interaction was waiting on it is a temporal overlap, "
            "not a demonstrated dependency";
        issue.cause_status = model::CauseStatus::kCandidate;

        // The list this rule exists to print. Naming the server would be the
        // easy conclusion and an unsupported one.
        issue.missing_evidence.push_back(
            "where the request's time went. A request duration covers DNS, "
            "the TLS handshake, a possibly cold radio, any retry, proxies, "
            "time queued behind other requests in the app, and the app's own "
            "delay in reading the response -- as well as the server. Nothing "
            "here separates them, so no server claim is made");
        issue.missing_evidence.push_back(
            "proof that the interaction was blocked on this request rather "
            "than merely running alongside it: that needs the app to mark the "
            "dependency, not a timing overlap");
        if (!interval.mapped) {
          issue.missing_evidence.push_back(
              "a measured mapping from the app's clock ('" + interval.domain +
              "') to the capture's timeline: the two spans are comparable "
              "with each other but cannot be lined up against device "
              "measurements");
        }
        for (const auto& fp : known_false_positives()) {
          issue.alternative_explanations.push_back(fp);
        }
        issue.suggested_verification.push_back(
            "record the same interaction with the request served from a warm "
            "cache, or with the dependency marked explicitly, and compare the "
            "interaction's own duration");

        if (share >= 0.9 && duration >= min_request * 3) {
          issue.severity = model::Severity::kMedium;
        } else {
          issue.severity = model::Severity::kLow;
        }
        issue.severity_rationale =
            "severity reflects how much of the interaction the request "
            "covered and how long it took. It is not a claim about the "
            "network, the server, or the user's perception";

        issue.threshold_expression =
            "request >= " + ms(min_request) + " and covers >= " +
            std::to_string(static_cast<int>(min_share * 100)) +
            "% of the interaction";
        issue.threshold_origin =
            "configurable_heuristic: what counts as a slow request is a "
            "product decision, not a platform constant";

        model::Metric metric;
        metric.name = "network.request_duration_ns";
        metric.unit = "ns";
        metric.value = static_cast<double>(duration);
        metric.provider = "app SDK";
        metric.method = model::MetricMethod::kMeasured;
        metric.aggregation = "single_request";
        metric.app_scoped = true;
        metric.window_start_ns = request_start;
        metric.window_end_ns = request_end;
        metric.limitations.push_back(
            "measured by the app around its own request call, so it includes "
            "the app's own queueing and response handling");
        metric.limitations.push_back(
            "not broken down into DNS, connect, TLS, first byte or transfer: "
            "the app reported one duration");
        issue.metrics.push_back(std::move(metric));

        model::EvidenceRef request_ref;
        request_ref.kind = "marker";
        request_ref.id = m.event_id;
        request_ref.start_ns = request_start;
        request_ref.end_ns = request_end;
        request_ref.note =
            (method.empty() ? std::string("request") : method) + " " +
            (url.empty() ? std::string("(url not reported)") : url) +
            " -- the URL is recorded as the app redacted it, without its query "
            "string";
        request_ref.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(request_ref));

        model::EvidenceRef interaction_ref;
        interaction_ref.kind = "marker";
        interaction_ref.id = interaction.marker->event_id;
        interaction_ref.start_ns = interaction.start_ns;
        interaction_ref.end_ns = interaction.end_ns;
        interaction_ref.note = "the interaction this request overlapped";
        interaction_ref.synthetic = t.synthetic;
        issue.evidence.push_back(std::move(interaction_ref));

        issue.fingerprint = make_fingerprint(id(), version(), interaction.label,
                                             url.empty() ? m.event_id : url);
        out.issues.push_back(std::move(issue));
      }
    }
  }
};

}  // namespace

RulePtr make_det11_network_delay() { return std::make_shared<Det11>(); }

}  // namespace mpi::rules
