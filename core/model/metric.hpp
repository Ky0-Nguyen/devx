// Metric semantics (spec section 8) and resource attribution (spec section 9).
//
// A bare number is not a metric. Every value carries its unit, provider,
// scope, window, how it was obtained, and what it cannot tell you.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model/event.hpp"
#include "core/util/json.hpp"

namespace mpi::model {

enum class MetricMethod { kMeasured, kSampled, kDerived };
const char* to_string(MetricMethod m);

// CPU percentage needs its normalization declared or it means nothing
// (spec section 8 / E12).
enum class CpuNormalization { kSingleCore, kAllCores, kNotApplicable, kUnknown };
const char* to_string(CpuNormalization n);

// Utilisation between two cumulative CPU-time readings, as a percentage of
// one core -- `CpuNormalization::kSingleCore`. All of a process's threads are
// counted together, so a multi-threaded app can legitimately exceed 100% on
// more than one core.
//
// The kernel publishes a counter, never a rate: `/proc/<pid>/stat` on Android
// and `proc_pid_rusage` on iOS both report CPU time accumulated since the
// process started. A rate is therefore always derived from two measured
// readings and the interval between them -- which is a difference of
// measurements, not an invention, provided the pair exists.
//
// Absent rather than zero when:
//   - there is no earlier reading to difference against, which is the state
//     of the first tick of every capture. Publishing 0% there would read as
//     an idle app at the one moment it certainly is not;
//   - the interval is not positive, so there is nothing to divide by;
//   - CPU time went backwards, which happens when a pid is reused between
//     readings. The honest answer then is that this pair does not describe
//     one process, not a negative utilisation.
std::optional<double> cpu_utilisation_percent(TimeNs earlier_at_ns,
                                              double earlier_cpu_ns,
                                              TimeNs at_ns, double cpu_ns);

struct Metric {
  std::string name;
  std::string unit;  // "ns", "ms", "bytes", "percent", "count", "hz"
  std::optional<double> value;  // absent means not measured, not zero
  std::string provider;

  // Scope: which process/thread the number belongs to. An app-level figure
  // only aggregates processes with established ownership.
  std::string process_instance_id;
  std::string thread_instance_id;
  bool app_scoped = false;

  TimeNs window_start_ns = 0;
  TimeNs window_end_ns = 0;

  MetricMethod method = MetricMethod::kDerived;
  std::string aggregation;  // "median", "p95", "sum", "max", "instant"
  CpuNormalization cpu_normalization = CpuNormalization::kNotApplicable;

  // Fraction of the window actually covered by the collector.
  std::optional<double> coverage_fraction;
  std::vector<std::string> limitations;
  std::vector<QualityFlag> quality_flags;

  bool measured() const { return value.has_value(); }
  json::Value to_json() const;
};

// Spec section 9. Keep device app, device collector, host desktop tool, and
// host Metro/IDE/debugger separate; never subtract overhead to guess release.
enum class AttributionCategory {
  kApplication,
  kDevelopmentTooling,
  kProfiler,
  kSharedOrUnknown,
};
const char* to_string(AttributionCategory c);

enum class AttributionBasis {
  kKnownOwnership,      // provider or SDK stated the owner
  kInstrumentation,     // our own instrumentation emitted it
  kMatchingStack,       // stack frames matched a versioned ownership rule
  kVerifiedEndpoint,    // a verified endpoint (e.g. Metro socket) accounted it
  kUnclassified,        // stays shared_or_unknown
};
const char* to_string(AttributionBasis b);

struct AttributedSlice {
  AttributionCategory category = AttributionCategory::kSharedOrUnknown;
  AttributionBasis basis = AttributionBasis::kUnclassified;
  std::string label;      // e.g. "metro_host_process", "mpi_recorder_buffers"
  std::string rule_id;    // versioned attribution rule that classified it
  std::string rule_version;
  std::optional<double> value;
  std::string unit;
  // Which host or device the slice lives on: an app's device CPU and Metro's
  // host CPU are different machines' resources (spec section 9 rule 1).
  std::string locus;  // "device.app" | "device.collector" | "host.tool" | "host.dev_server"
  std::vector<std::string> evidence_refs;
  json::Value to_json() const;
};

// The original total is always preserved next to any attributed subset, and
// the subset is never presented as subtractable (spec section 9 rules 6, 7, 9).
struct AttributionReport {
  Metric original_total;
  std::vector<AttributedSlice> slices;
  // Inclusive stack percentages cannot be summed as disjoint costs (F17).
  bool slices_are_disjoint = false;
  std::vector<std::string> limitations;

  json::Value to_json() const;
};

}  // namespace mpi::model
