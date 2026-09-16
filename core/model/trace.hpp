// The normalized trace: the single input every analysis rule reads.
//
// Platform adapters differ wildly; this container does not. Anything a rule
// needs to know about provenance, coverage, clocks, ownership, or build state
// travels inside it, so the analysis engine never has to ask the device.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model/build.hpp"
#include "core/model/capability.hpp"
#include "core/model/event.hpp"
#include "core/model/identity.hpp"
#include "core/model/metric.hpp"
#include "core/util/json.hpp"

namespace mpi::model {

// A display refresh regime observed during the capture. There is no universal
// 16 ms rule (spec section 8): the deadline comes from the observed rate, and
// a change mid-session produces a second interval (spec E02).
struct RefreshInterval {
  TimeNs start_ns = 0;
  TimeNs end_ns = 0;
  std::optional<double> hz;
  // True when the rate is variable and a single deadline is not defensible.
  bool variable = false;
  std::string provider;

  std::optional<TimeNs> deadline_ns() const;
  json::Value to_json() const;
};

// How frame timing was obtained. A display-callback proxy is not presentation
// truth and must stay labelled as a proxy (spec E21).
enum class FrameSource {
  kPresentationTimestamps,  // actual presentation evidence
  kFrameDeadlineReports,    // provider-reported deadline/miss records
  kDisplayCallbackProxy,    // proxy only
  kUnknown,
};
const char* to_string(FrameSource s);

struct FrameRecord {
  std::string event_id;
  TimeNs start_ns = 0;
  std::optional<TimeNs> presented_ns;
  std::optional<TimeNs> deadline_ns;
  std::optional<TimeNs> cpu_duration_ns;
  FrameSource source = FrameSource::kUnknown;
  std::string process_instance_id;
  std::string surface;  // which app surface/layer (spec E19)
  std::vector<QualityFlag> quality_flags;

  // Only answerable when both a presentation time and a deadline exist.
  std::optional<bool> missed_deadline() const;
  std::optional<TimeNs> overrun_ns() const;
  json::Value to_json() const;
};

// One sampled stack. Samples estimate attribution; they are not exact
// function durations, and a missing sample is not idle time (spec section 8).
struct CpuSample {
  TimeNs timestamp_ns = 0;
  std::string process_instance_id;
  std::string thread_instance_id;
  // Outermost frame first. Strings are raw provider symbols until the symbol
  // service resolves them.
  std::vector<std::string> frames;
  std::optional<double> weight;  // sample weight when the provider gives one
  std::string provider;
  json::Value to_json() const;
};

// A JS task / Hermes execution span.
struct JsTask {
  std::string event_id;
  TimeNs start_ns = 0;
  std::optional<TimeNs> duration_ns;
  std::string name;
  std::string process_instance_id;
  std::string thread_instance_id;
  std::string clock_domain;
  // True when the JS clock has a measured mapping to the UI clock. Without
  // it, no claim about UI impact can be made (spec section 11).
  bool clock_mapped_to_ui = false;
  std::vector<QualityFlag> quality_flags;
  json::Value to_json() const;
};

// SDK marker: screen mount/unmount, navigation begin/end/cancel, interaction.
struct Marker {
  std::string event_id;
  TimeNs timestamp_ns = 0;
  std::optional<TimeNs> duration_ns;
  std::string kind;  // "screen_mount" | "navigation_begin" | "interaction" ...
  std::string screen;
  std::string interaction;
  std::string process_instance_id;
  // Which clock `timestamp_ns` is on. An SDK marker is stamped by the app, on
  // the app's own clock, and comparing it against a device-clock measurement
  // without a mapping would correlate two unrelated timelines (spec section
  // 6, section 11). Empty means the capture's primary domain.
  std::string clock_domain;
  json::Value payload = json::Value::object();
  json::Value to_json() const;
};

struct CounterSeries {
  std::string name;   // "memory.rss_bytes", "memory.js_heap_used_bytes"
  std::string unit;
  std::string provider;
  std::string process_instance_id;
  // Distinct metric families are never merged or summed (spec section 8).
  std::string family;  // "rss" | "pss" | "private_dirty" | "footprint" | "js_heap"

  // Whether each point is a running total or a reading taken at that instant.
  //
  // The distinction decides which number means anything. For a cumulative
  // series the quantity of interest is the difference between two points --
  // `cpu.process_time_ns` is counted from when the process started, so a
  // 67-second capture of an app that had been running for hours opened at
  // 3.18 h, a true figure that says nothing about the capture. For an
  // instantaneous series it is the point itself; differencing rss across a
  // window answers a different question than reading it.
  //
  // A reader cannot infer this from the name, which is why it is declared
  // rather than guessed at by suffix.
  bool cumulative = false;

  // What a percentage is a percentage OF. A CPU percentage without its
  // normalization declared means nothing (spec section 8 / E12): on an
  // eight-core device, 100% is either one core saturated or the whole SoC.
  // `Metric` has carried this since the beginning; a counter series
  // published one without it.
  CpuNormalization cpu_normalization = CpuNormalization::kNotApplicable;

  std::vector<std::pair<TimeNs, double>> points;
  json::Value to_json() const;
};

struct TargetSelection {
  ApplicationKey app;
  std::vector<ProcessInstance> processes;
  RuntimeState runtime_state_at_capture = RuntimeState::kUnknown;
  DiscoveryScope discovery_scope = DiscoveryScope::kUnknown;
  json::Value to_json() const;
};

struct NormalizedTrace {
  std::string session_id;
  std::string schema_version = "2.0";
  // True for every fixture-sourced trace. Set once, propagated everywhere.
  bool synthetic = false;
  std::string synthetic_note;

  DeviceRef device;
  TargetSelection target;
  BuildProfile build;
  CapabilityMatrix capabilities;
  MeasurementMode requested_mode = MeasurementMode::kDiagnostic;

  std::vector<ClockDomain> clock_domains;
  std::vector<ClockMapping> clock_mappings;
  // The domain durations and windows are expressed in after normalization.
  std::string primary_clock_domain;

  TimeNs window_start_ns = 0;
  TimeNs window_end_ns = 0;

  std::vector<ThreadInfo> threads;
  std::vector<Event> events;
  std::vector<FrameRecord> frames;
  std::vector<CpuSample> cpu_samples;
  std::vector<JsTask> js_tasks;
  std::vector<Marker> markers;
  std::vector<CounterSeries> counters;
  std::vector<RefreshInterval> refresh_intervals;
  std::vector<Coverage> coverage;
  // Provider-reported drop counts, kept visible (spec D09).
  std::map<std::string, std::int64_t> dropped_events_by_collector;

  std::vector<std::string> ingestion_warnings;
  // Set when the capture ended abnormally; a partial trace never yields a
  // clean pass (spec D19).
  bool partial = false;
  std::vector<std::string> partial_reasons;

  const ThreadInfo* thread(const std::string& id) const;
  const Coverage* coverage_for(const std::string& collector) const;
  // Nullopt when no mapping exists: unmapped clocks are not silently aligned.
  std::optional<TimeNs> map_to_primary(const std::string& domain,
                                       TimeNs t) const;
  // The refresh interval covering `t`, if the display rate was observed.
  const RefreshInterval* refresh_at(TimeNs t) const;

  TimeNs duration_ns() const { return window_end_ns - window_start_ns; }
  json::Value to_json(bool include_events) const;
};

}  // namespace mpi::model
