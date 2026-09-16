// DET-05 -- memory growth across screen cycles.
//
// The shape this looks for is simple: mount a screen, leave it, come back,
// and see whether memory after each visit is higher than after the last. What
// makes the rule difficult is that the same shape has several innocent causes,
// and the spec is explicit that the strongest allowed conclusion is
// *suspected retention* (section 10.2, DET-05).
//
// So this rule is built to refuse the word "leak". Rising memory across
// cycles is equally consistent with:
//
//   * a cache filling up on purpose,
//   * garbage that exists but has not been collected yet,
//   * an allocator holding freed pages rather than returning them,
//   * the screen legitimately keeping more state after being visited.
//
// All four are listed on every finding. What the rule contributes is the
// measurement: which family grew, by how much, across how many comparable
// cycles -- and how coarse the sampling was underneath that claim.
//
// Two things it will not do. It never sums memory families (spec section 8):
// RSS, PSS, private-dirty and the heaps are separate measurements, so each is
// evaluated on its own and a finding names which one grew. And it needs the
// *app's own* cycle markers: without them there is no way to know a screen was
// revisited, and guessing a screen from anything else is forbidden.
#include <algorithm>
#include <cmath>

#include "core/rules/rule.hpp"

namespace mpi::rules {
namespace {

std::string mib(double bytes) {
  const double value = bytes / (1024.0 * 1024.0);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f MiB", value);
  return std::string(buf);
}

// One completed visit to a screen: mounted, then unmounted.
struct Cycle {
  std::string screen;
  model::TimeNs mounted_ns = 0;
  model::TimeNs unmounted_ns = 0;
  // The clock the markers were stamped on, which for an SDK marker is the
  // app's own and not the device's.
  std::string clock_domain;
};

std::vector<Cycle> completed_cycles(const model::NormalizedTrace& t) {
  std::vector<Cycle> cycles;
  // Markers arrive in order; an unmount closes the most recent open mount of
  // the same screen. A mount with no unmount is an open visit and is not a
  // cycle: comparing a screen that is still on display against one that has
  // been left is not comparing like with like.
  std::vector<Cycle> open;
  for (const auto& m : t.markers) {
    if (m.screen.empty()) continue;
    if (m.kind == "screen_mount") {
      open.push_back(Cycle{m.screen, m.timestamp_ns, 0, m.clock_domain});
      continue;
    }
    if (m.kind != "screen_unmount") continue;
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
      if (it->screen != m.screen || it->unmounted_ns != 0) continue;
      it->unmounted_ns = m.timestamp_ns;
      cycles.push_back(*it);
      break;
    }
  }
  std::sort(cycles.begin(), cycles.end(), [](const Cycle& a, const Cycle& b) {
    return a.unmounted_ns < b.unmounted_ns;
  });
  return cycles;
}

// The counter reading at or just before an instant, with how stale it is.
// Whether a series is one this rule can read as memory held.
//
// The loop below iterated every counter in the trace, which was correct while
// the only counters were memory families and wrong as soon as anything else
// existed. `cpu.process_time_ns` rises monotonically by construction -- it is
// a running total -- so it presented as a perfectly clean growth trend across
// every cycle, and the thresholds it was tested against are sizes in bytes
// being compared to nanoseconds. A percentage series would have been read the
// same way.
//
// Two conditions, because either alone would let something through: a size
// that is a running total is not memory held at an instant, and a rate is not
// a size however it moves.
bool is_memory_series(const model::CounterSeries& s) {
  return s.unit == "bytes" && !s.cumulative;
}

struct Reading {
  double value = 0.0;
  model::TimeNs at_ns = 0;
  model::TimeNs staleness_ns = 0;
  bool found = false;
};

Reading reading_at(const model::CounterSeries& series, model::TimeNs at_ns) {
  Reading best;
  for (const auto& point : series.points) {
    if (point.first <= 0 || point.first > at_ns) continue;
    if (!best.found || point.first > best.at_ns) {
      best.found = true;
      best.at_ns = point.first;
      best.value = point.second;
    }
  }
  if (best.found) best.staleness_ns = at_ns - best.at_ns;
  return best;
}

// A cycle's boundary on the capture's own timeline, or nothing.
//
// This is the check that keeps the rule honest. SDK markers are stamped by
// the app, and a counter is stamped by the device; comparing the two numbers
// directly would correlate two unrelated timelines. `map_to_primary` applies
// only a *measured* mapping and refuses an assumed one (spec section 6).
std::optional<model::TimeNs> boundary_on_primary(
    const model::NormalizedTrace& t, const Cycle& cycle) {
  if (cycle.clock_domain.empty()) return cycle.unmounted_ns;
  return t.map_to_primary(cycle.clock_domain, cycle.unmounted_ns);
}

class Det05 final : public Rule {
 public:
  std::string id() const override { return "DET-05"; }
  std::string version() const override { return "1"; }
  std::string category() const override { return "memory"; }
  std::string title() const override {
    return "Memory growth across screen cycles";
  }
  std::string delivery_phase() const override { return "M5"; }

  std::vector<Prerequisite> prerequisites() const override {
    return {
        {"screen_cycles",
         "at least three completed mount/unmount cycles of the same screen, "
         "from the app's own SDK markers"},
        {"memory_series",
         "a memory counter series with a reading inside each cycle"},
        {"comparable_checkpoints",
         "the cycles must be of the same screen in the same process, so the "
         "readings are comparable"},
    };
  }

  std::vector<ThresholdSpec> thresholds() const override {
    return {
        ThresholdSpec{"min_cycles", 3, "count",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "below three visits a rise is one difference, not a "
                      "trend; raise it for a stronger claim"},
        ThresholdSpec{"min_growth_bytes_per_cycle", 2.0 * 1024 * 1024, "bytes",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "initial default only. There is no universal memory "
                      "ceiling and this is not one: it is the smallest "
                      "per-cycle rise worth reporting for a typical app"},
        ThresholdSpec{"min_relative_growth", 0.10, "fraction",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "the rise must also be this large relative to the first "
                      "reading, so a big app is not flagged for noise"},
        ThresholdSpec{"max_staleness_ms", 2000, "ms",
                      ThresholdOrigin::kConfigurableHeuristic,
                      "a reading older than this relative to the cycle "
                      "boundary is not treated as that cycle's memory"},
    };
  }

  std::vector<std::string> known_false_positives() const override {
    return {
        "a cache filling up on purpose produces exactly this shape, and this "
        "rule cannot tell a cache from a retention",
        "garbage that exists but has not been collected yet: without a forced "
        "collection before each reading, the rise may be entirely collectable",
        "an allocator holding freed pages instead of returning them to the "
        "OS, which raises RSS while the app's live set is unchanged",
        "a screen that legitimately keeps more state after being visited, "
        "such as a list that has loaded more pages",
    };
  }

  std::vector<std::string> unmet_prerequisites(const RuleContext& ctx) const override {
    std::vector<std::string> unmet;
    const auto& t = *ctx.trace;
    const auto min_cycles = static_cast<std::size_t>(
        ctx.override_for("DET-05.min_cycles").value_or(3.0));

    const auto cycles = completed_cycles(t);
    if (cycles.empty()) {
      unmet.push_back(
          "no completed screen mount/unmount cycle was recorded. These come "
          "from the app's own SDK markers, so record with --sdk and have the "
          "app report screen lifecycle; a screen cannot be inferred from "
          "anything else (spec section 11)");
    } else {
      // Per screen, because cycles of different screens are not comparable
      // checkpoints.
      std::size_t best = 0;
      for (const auto& candidate : cycles) {
        const auto count = static_cast<std::size_t>(std::count_if(
            cycles.begin(), cycles.end(), [&](const Cycle& c) {
              return c.screen == candidate.screen;
            }));
        best = std::max(best, count);
      }
      if (best < min_cycles) {
        unmet.push_back("the most-visited screen has " + std::to_string(best) +
                        " completed cycle(s), below the min_cycles threshold "
                        "of " + std::to_string(min_cycles) +
                        "; a rise across fewer visits is a difference, not a "
                        "trend");
      }
    }

    if (std::none_of(t.counters.begin(), t.counters.end(),
                     is_memory_series)) {
      unmet.push_back(
          "no memory counter series was collected, so there is nothing to "
          "compare across cycles" +
          std::string(t.counters.empty()
                          ? ""
                          : " (the capture has counters, but none of them is "
                            "a size held at an instant -- CPU time is a "
                            "running total and a utilisation is a rate)"));
    }

    // The clocks have to be related by measurement. Without that, the cycle
    // boundaries and the memory readings are numbers from two different
    // timelines and comparing them would invent a correlation.
    if (!cycles.empty()) {
      const bool mappable = std::any_of(
          cycles.begin(), cycles.end(), [&](const Cycle& c) {
            return boundary_on_primary(t, c).has_value();
          });
      if (!mappable) {
        const std::string domain = cycles.front().clock_domain.empty()
                                       ? std::string("(unstated)")
                                       : cycles.front().clock_domain;
        unmet.push_back(
            "the screen markers are on clock '" + domain +
            "' and the capture's timeline is '" + t.primary_clock_domain +
            "', with no measured mapping between them. The app's timestamps "
            "and the device's memory readings cannot be compared without one, "
            "and an assumed offset would make a correlation look real");
      }
    }
    return unmet;
  }

  void evaluate(const RuleContext& ctx, RuleOutput& out) const override {
    const auto& t = *ctx.trace;
    const auto min_cycles = static_cast<std::size_t>(
        ctx.override_for("DET-05.min_cycles").value_or(3.0));
    const double min_growth =
        ctx.override_for("DET-05.min_growth_bytes_per_cycle")
            .value_or(2.0 * 1024 * 1024);
    const double min_relative =
        ctx.override_for("DET-05.min_relative_growth").value_or(0.10);
    const auto max_staleness = static_cast<model::TimeNs>(
        ctx.override_for("DET-05.max_staleness_ms").value_or(2000.0) *
        1000000.0);

    const auto cycles = completed_cycles(t);
    // Screens in first-visit order, so the report is stable.
    std::vector<std::string> screens;
    for (const auto& c : cycles) {
      if (std::find(screens.begin(), screens.end(), c.screen) == screens.end()) {
        screens.push_back(c.screen);
      }
    }

    for (const auto& screen : screens) {
      if (ctx.cancel.cancelled()) return;
      std::vector<Cycle> visits;
      for (const auto& c : cycles) {
        if (c.screen == screen) visits.push_back(c);
      }
      if (visits.size() < min_cycles) {
        out.record.skipped_reasons.push_back(
            "screen '" + screen + "' had " + std::to_string(visits.size()) +
            " completed cycle(s), below min_cycles");
        continue;
      }

      // Each family separately. Summing them would be meaningless: PSS and
      // RSS measure overlapping things, and private-dirty is a subset of
      // both (spec section 8).
      for (const auto& series : t.counters) {
        if (ctx.cancel.cancelled()) return;
        // Only what this rule can honestly read as memory. Not recorded as a
        // skipped reason: a CPU counter was never a candidate for a memory
        // finding, so naming it here would be noise rather than a gap.
        if (!is_memory_series(series)) continue;
        if (series.points.size() < min_cycles) continue;

        std::vector<Reading> readings;
        std::size_t stale = 0;
        std::size_t unmapped = 0;
        for (const auto& visit : visits) {
          const auto boundary = boundary_on_primary(t, visit);
          if (!boundary.has_value()) {
            ++unmapped;
            continue;
          }
          auto r = reading_at(series, *boundary);
          if (!r.found) continue;
          if (r.staleness_ns > max_staleness) {
            ++stale;
            continue;
          }
          readings.push_back(r);
        }
        if (readings.size() < min_cycles) {
          out.record.skipped_reasons.push_back(
              "series '" + series.name + "' on screen '" + screen +
              "' had a usable reading for only " +
              std::to_string(readings.size()) + " of " +
              std::to_string(visits.size()) +
              " cycle(s)" +
              (stale > 0 ? " (" + std::to_string(stale) +
                               " reading(s) were too old to attribute to a "
                               "cycle)"
                         : "") +
              (unmapped > 0 ? " (" + std::to_string(unmapped) +
                                  " cycle boundary/ies could not be placed on "
                                  "the capture's timeline)"
                            : "") +
              "; the memory sampling cadence is coarser than these cycles");
          continue;
        }

        const double first = readings.front().value;
        const double last = readings.back().value;
        const double total_growth = last - first;
        if (total_growth <= 0.0) continue;
        const auto steps = static_cast<double>(readings.size() - 1);
        const double per_cycle = total_growth / steps;
        const double relative = first > 0.0 ? total_growth / first : 0.0;
        if (per_cycle < min_growth || relative < min_relative) continue;

        // How many steps actually rose. A series that rises overall but
        // wobbles is weaker evidence than one that rises every time, and the
        // finding says which it was rather than averaging the distinction
        // away.
        std::size_t rising_steps = 0;
        for (std::size_t i = 1; i < readings.size(); ++i) {
          if (readings[i].value > readings[i - 1].value) ++rising_steps;
        }
        const bool monotonic = rising_steps == readings.size() - 1;

        // Warm-up, which is the shape this rule most easily mistakes for
        // retention (spec F02). An app that fills a cache on its first visits
        // and then holds steady rises overall while retaining nothing new, so
        // the later half of the cycles is checked on its own: if it is flat,
        // the first-to-last difference is a warm-up and saying "suspected
        // retention" would be wrong rather than merely cautious.
        if (readings.size() >= 4) {
          const std::size_t mid = readings.size() / 2;
          const double late_growth = last - readings[mid].value;
          const double early_growth = readings[mid].value - first;
          if (early_growth > 0.0 && late_growth <= early_growth * 0.2) {
            out.record.skipped_reasons.push_back(
                "series '" + series.name + "' on screen '" + screen +
                "' rose " + mib(early_growth) + " over the first " +
                std::to_string(mid) + " visit(s) and only " +
                mib(late_growth) + " over the remaining " +
                std::to_string(readings.size() - 1 - mid) +
                ": that is warm-up settling, not accumulation across cycles, "
                "and reporting it as suspected retention would be wrong");
            continue;
          }
        }

        model::Issue issue;
        issue.rule_id = id();
        issue.rule_version = version();
        issue.session_id = t.session_id;
        issue.category = category();
        issue.mode = ctx.mode;
        issue.eligibility = ctx.eligibility;
        issue.screen = screen;
        issue.process_instance_id = series.process_instance_id;
        // Reported on the capture's timeline, using the mapped boundaries:
        // the raw marker numbers belong to the app's clock.
        issue.start_ns = readings.front().at_ns;
        issue.end_ns = readings.back().at_ns;

        issue.title = series.family.empty() ? series.name : series.family;
        issue.title += " grew " + mib(total_growth) + " across " +
                       std::to_string(readings.size()) + " visits to '" +
                       screen + "'";

        // Suspected, never observed. The growth is measured; that it is
        // retention is not, and the spec caps this rule at suspected.
        issue.detection_status = model::DetectionStatus::kSuspected;
        issue.confidence_basis =
            "the growth itself is measured -- " + mib(first) + " after the "
            "first visit, " + mib(last) + " after the last, over " +
            std::to_string(readings.size()) +
            " comparable cycles of the same screen in the same process. That "
            "it is retention is not measured: this rule cannot distinguish "
            "retention from a cache, uncollected garbage, or an allocator "
            "holding pages";
        if (!monotonic) {
          issue.confidence_basis +=
              ". The rise was not monotonic (" + std::to_string(rising_steps) +
              " of " + std::to_string(readings.size() - 1) +
              " steps rose), which weakens it further";
        }

        issue.cause_status = model::CauseStatus::kUnknown;
        issue.missing_evidence.push_back(
            "a heap snapshot with reference paths: growth alone cannot name "
            "what is held or who holds it (that is DET-06, unimplemented)");
        issue.missing_evidence.push_back(
            "a forced garbage collection before each reading, without which "
            "the rise may be entirely collectable");
        for (const auto& fp : known_false_positives()) {
          issue.alternative_explanations.push_back(fp);
        }
        issue.suggested_verification.push_back(
            "repeat the same visits with a forced collection before each "
            "reading; if the rise survives that, capture a heap snapshot at "
            "the first and last visit and compare what is held");
        issue.proposed_remediation.push_back(
            "before treating this as a leak, check whether the screen owns a "
            "cache and what bounds it");

        // Severity from the size of the rise, and deliberately capped: a
        // suspected finding should not outrank a measured one.
        if (relative >= 0.5 && monotonic) {
          issue.severity = model::Severity::kMedium;
        } else {
          issue.severity = model::Severity::kLow;
        }
        issue.severity_rationale =
            "severity reflects how large and how consistent the rise was. It "
            "is capped at medium because the finding is suspected, not "
            "observed: a measured symptom should always outrank an inferred "
            "one";

        issue.threshold_expression =
            "growth per cycle >= " + mib(min_growth) + " and relative growth "
            ">= " + std::to_string(min_relative) + " over " +
            std::to_string(min_cycles) + "+ cycles";
        issue.threshold_origin =
            "configurable_heuristic: there is no universal memory ceiling, and "
            "these are starting values to tune per app, not a standard";
        issue.baseline_value = first;

        model::Metric growth;
        growth.name = series.name + ".growth_across_cycles_bytes";
        growth.unit = "bytes";
        growth.value = total_growth;
        growth.provider = series.provider;
        growth.process_instance_id = series.process_instance_id;
        growth.method = model::MetricMethod::kDerived;
        growth.aggregation = "last_cycle_reading_minus_first";
        growth.app_scoped = true;
        growth.window_start_ns = issue.start_ns;
        growth.window_end_ns = issue.end_ns;
        growth.limitations.push_back(
            "one reading per cycle boundary, not a continuous measurement: "
            "what happened between readings is not observed");
        growth.limitations.push_back(
            "this family is never summed with another (spec section 8)");
        if (!visits.front().clock_domain.empty()) {
          for (const auto& mapping : t.clock_mappings) {
            if (mapping.from_domain != visits.front().clock_domain) continue;
            if (!mapping.uncertainty_ns.has_value()) continue;
            growth.limitations.push_back(
                "the cycle boundaries came from the app's clock and were "
                "mapped onto the capture's timeline to within +/- " +
                std::to_string(*mapping.uncertainty_ns / 1000000) + " ms");
          }
        }
        if (stale > 0) {
          growth.limitations.push_back(
              std::to_string(stale) +
              " cycle(s) had no reading close enough to their boundary and "
              "were left out");
        }
        issue.metrics.push_back(std::move(growth));

        for (std::size_t i = 0; i < readings.size(); ++i) {
          model::EvidenceRef ref;
          ref.kind = "counter";
          ref.id = series.name;
          ref.start_ns = readings[i].at_ns;
          ref.note = "visit " + std::to_string(i + 1) + ": " +
                     mib(readings[i].value) + " (reading taken " +
                     std::to_string(readings[i].staleness_ns / 1000000) +
                     " ms before the unmount)";
          ref.synthetic = t.synthetic;
          issue.evidence.push_back(std::move(ref));
        }

        issue.fingerprint =
            make_fingerprint(id(), version(), screen, series.name);
        out.issues.push_back(std::move(issue));
      }
    }
  }
};

}  // namespace

RulePtr make_det05_memory_growth() { return std::make_shared<Det05>(); }

}  // namespace mpi::rules
