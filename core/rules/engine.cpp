#include "core/rules/engine.hpp"

#include <algorithm>
#include <set>

#include "core/rules/rule_registry.hpp"
#include "core/util/time.hpp"

namespace mpi::rules {

namespace {

// Whether `expiry` is in the past relative to `now`, both ISO-8601 UTC.
//
// Compared on the date prefix only. An expiry is a project decision recorded
// by a person, and "2026-09-30" is the form people write; demanding a full
// timestamp would make a perfectly clear expiry unparseable, and treating an
// unparseable one as expired would silently un-suppress. So: unparseable
// means "no expiry I can judge", which keeps the suppression and is reported.
enum class ExpiryState { kNone, kUnparseable, kActive, kExpired };

ExpiryState classify_expiry(const std::string& expiry, const std::string& now) {
  if (expiry.empty()) return ExpiryState::kNone;
  const auto date_of = [](const std::string& s) -> std::string {
    if (s.size() < 10) return {};
    for (std::size_t i = 0; i < 10; ++i) {
      const char c = s[i];
      const bool ok = (i == 4 || i == 7) ? (c == '-') : (c >= '0' && c <= '9');
      if (!ok) return {};
    }
    return s.substr(0, 10);
  };
  const std::string a = date_of(expiry);
  const std::string b = date_of(now);
  if (a.empty() || b.empty()) return ExpiryState::kUnparseable;
  // Zero-padded ISO dates compare correctly as strings.
  return a < b ? ExpiryState::kExpired : ExpiryState::kActive;
}

}  // namespace


std::string engine_version() { return "0.3.0"; }

model::AnalysisResult analyze(const model::NormalizedTrace& trace,
                              const symbols::SymbolService& symbol_service,
                              const EngineOptions& opts) {
  model::AnalysisResult result;
  result.session_id = trace.session_id;
  result.engine_version = engine_version();
  result.ruleset_version = ruleset_version();
  result.analyzed_at = time_util::now_iso8601_utc();
  result.mode = opts.mode;
  result.contains_synthetic_data = trace.synthetic;
  result.coverage = trace.coverage;

  // Eligibility is computed once, from the build profile, and stamped onto
  // every issue. A diagnostic session can never come out as a benchmark pass.
  result.eligibility = model::evaluate_eligibility(trace.build, opts.mode);

  if (trace.synthetic) {
    result.data_quality_notes.push_back(
        "SYNTHETIC INPUT: this analysis ran on a labelled fixture, not a real "
        "device capture. No conclusion here describes real app behavior." +
        (trace.synthetic_note.empty() ? std::string()
                                      : " Note: " + trace.synthetic_note));
  }
  if (trace.partial) {
    result.data_quality_notes.push_back(
        "PARTIAL CAPTURE: the recording ended abnormally, so absence of a "
        "finding is not evidence of absence.");
    for (const auto& r : trace.partial_reasons) {
      result.data_quality_notes.push_back("partial capture reason: " + r);
    }
  }
  for (const auto& w : trace.ingestion_warnings) {
    result.data_quality_notes.push_back("ingestion: " + w);
  }
  for (const auto& kv : trace.dropped_events_by_collector) {
    if (kv.second <= 0) continue;
    result.data_quality_notes.push_back(
        kv.first + " dropped " + std::to_string(kv.second) +
        " event(s); the affected intervals are gaps, not measured zero "
        "activity");
  }
  // Ambiguous process ownership is stated rather than folded into totals.
  for (const auto& p : trace.target.processes) {
    if (p.counts_toward_app_totals()) continue;
    result.data_quality_notes.push_back(
        "process " + std::to_string(p.pid) + " (" + p.process_name +
        ") has " + model::to_string(p.ownership) +
        " ownership evidence and is excluded from app-scoped totals" +
        (p.ownership_note.empty() ? std::string() : ": " + p.ownership_note));
  }
  if (trace.target.discovery_scope == model::DiscoveryScope::kPartial) {
    result.data_quality_notes.push_back(
        "the app list this target came from was a partial listing, so other "
        "processes of this app may exist unobserved");
  }

  RuleContext ctx;
  ctx.trace = &trace;
  ctx.symbol_service = &symbol_service;
  ctx.mode = opts.mode;
  ctx.heap_graph = opts.heap_graph;
  ctx.eligibility = result.eligibility;
  ctx.cancel = opts.cancel;
  ctx.threshold_overrides = opts.threshold_overrides;

  // Judged once for the whole analysis, so two issues from the same run
  // cannot disagree about whether a suppression had lapsed.
  const std::string now = opts.evaluated_at.empty()
                              ? time_util::now_iso8601_utc()
                              : opts.evaluated_at;
  std::set<std::string> expired_suppressions;
  std::set<std::string> unparseable_expiries;

  for (const auto& rule : all_rules()) {
    if (opts.cancel.cancelled()) {
      model::RuleRunRecord rec;
      rec.rule_id = rule->id();
      rec.rule_version = rule->version();
      rec.outcome = model::RuleOutcome::kSkipped;
      rec.skipped_reasons.push_back("analysis cancelled before this rule ran");
      result.rule_runs.push_back(std::move(rec));
      continue;
    }
    RuleOutput out = rule->run(ctx);
    for (auto& issue : out.issues) {
      // Suppressions are applied here, never by dropping the issue: the
      // reason and expiry travel into the export (spec section 13, H10).
      for (const auto& s : opts.suppressions) {
        if (s.rule_id != issue.rule_id) continue;
        if (!s.fingerprint.empty() && s.fingerprint != issue.fingerprint) continue;
        const auto state = classify_expiry(s.expiry, now);
        if (state == ExpiryState::kExpired) {
          // An expiry that never expires is worse than no expiry: it creates
          // the belief that suppressions lapse. So the issue is reported, and
          // the lapsed suppression is named -- silently un-suppressing would
          // leave someone wondering why a finding came back.
          expired_suppressions.insert(
              s.rule_id + " (expired " + s.expiry + ", reason: " + s.reason +
              ")");
          continue;
        }
        issue.suppressed = true;
        issue.suppression_reason = s.reason;
        issue.suppression_expiry = s.expiry;
        issue.suppression_author = s.author;
        if (state == ExpiryState::kUnparseable) {
          unparseable_expiries.insert(s.rule_id + " (expiry '" + s.expiry +
                                      "' is not an ISO-8601 date)");
        }
        break;
      }
      result.issues.push_back(std::move(issue));
    }
    for (auto& a : out.attribution) result.attribution.push_back(std::move(a));
    result.rule_runs.push_back(std::move(out.record));
  }

  for (const auto& e : expired_suppressions) {
    result.data_quality_notes.push_back(
        "a suppression was NOT applied because it had expired: " + e +
        ". The finding it covered is reported above");
  }
  for (const auto& u : unparseable_expiries) {
    result.data_quality_notes.push_back(
        "a suppression's expiry could not be read as a date, so it was "
        "treated as having none and the suppression still applies: " + u);
  }

  // Impact order for display; ties broken by fingerprint so the order is
  // stable across re-analysis of the same input.
  std::stable_sort(result.issues.begin(), result.issues.end(),
                   [](const model::Issue& a, const model::Issue& b) {
                     if (a.severity != b.severity) {
                       return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                     }
                     return a.fingerprint < b.fingerprint;
                   });
  return result;
}

model::AnalysisResult analyze_comparison(const RegressionInput& comparison,
                                         const EngineOptions& opts) {
  model::AnalysisResult result;
  result.engine_version = engine_version();
  result.ruleset_version = ruleset_version();
  result.analyzed_at = time_util::now_iso8601_utc();
  result.mode = opts.mode;

  // A comparison's eligibility is a property of its two run sets, which the
  // comparison engine has already judged per metric. Nothing is recomputed
  // from a build profile here, because there is no single build.
  bool any_uncertified = false;
  for (const auto& m : comparison.metrics) {
    if (!m.certified) any_uncertified = true;
  }
  if (any_uncertified) {
    result.data_quality_notes.push_back(
        "at least one metric in this comparison is not a certified benchmark "
        "pair: it measures a real difference between these two "
        "configurations and certifies nothing about release performance");
  }
  for (const auto& reason : comparison.incompatibilities) {
    result.data_quality_notes.push_back(
        "run conditions are not comparable: " + reason);
  }
  if (comparison.cross_platform) {
    result.data_quality_notes.push_back(
        "this pair spans two platforms; it may be displayed side by side but "
        "cannot drive a regression gate");
  }

  // An empty trace rather than a null one: a rule reaching for a capture
  // finds nothing there instead of dereferencing nothing at all.
  const model::NormalizedTrace empty_trace;
  const symbols::SymbolService no_symbols;

  RuleContext ctx;
  ctx.trace = &empty_trace;
  ctx.symbol_service = &no_symbols;
  ctx.regression = &comparison;
  ctx.mode = opts.mode;
  ctx.cancel = opts.cancel;
  ctx.threshold_overrides = opts.threshold_overrides;

  for (const auto& rule : all_rules()) {
    if (!rule->uses_comparison_input()) continue;
    if (opts.cancel.cancelled()) {
      model::RuleRunRecord rec;
      rec.rule_id = rule->id();
      rec.rule_version = rule->version();
      rec.outcome = model::RuleOutcome::kSkipped;
      rec.skipped_reasons.push_back("analysis cancelled before this rule ran");
      result.rule_runs.push_back(std::move(rec));
      continue;
    }
    RuleOutput out = rule->run(ctx);
    for (auto& issue : out.issues) {
      for (const auto& s : opts.suppressions) {
        if (s.rule_id != issue.rule_id) continue;
        if (!s.fingerprint.empty() && s.fingerprint != issue.fingerprint) continue;
        issue.suppressed = true;
        issue.suppression_reason = s.reason;
        issue.suppression_expiry = s.expiry;
        issue.suppression_author = s.author;
        break;
      }
      result.issues.push_back(std::move(issue));
    }
    result.rule_runs.push_back(std::move(out.record));
  }

  std::stable_sort(result.issues.begin(), result.issues.end(),
                   [](const model::Issue& a, const model::Issue& b) {
                     if (a.severity != b.severity) {
                       return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                     }
                     return a.fingerprint < b.fingerprint;
                   });
  return result;
}

}  // namespace mpi::rules
