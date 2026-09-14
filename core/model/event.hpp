// Normalized event model (spec section 6).
//
// Durations come from monotonic timestamps. Clock domains are explicit and
// carry their mapping uncertainty; nothing is correlated by wall-clock
// equality alone. A missing event is a gap, never zero resource usage.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::model {

// Nanoseconds on the domain's own monotonic base.
using TimeNs = std::int64_t;

// Which clock a timestamp is expressed on. Timestamps from different domains
// are never subtracted without a ClockMapping.
struct ClockDomain {
  std::string id;        // "android.boottime", "ios.mach_absolute", "js.hermes"
  std::string base;      // "monotonic" | "wall" | "unknown"
  std::string provider;
  bool monotonic = false;

  json::Value to_json() const;
};

// A measured mapping between two clock domains, with the uncertainty kept.
// Spec section 6: preserve uncertainty; never correlate solely by wall-clock.
struct ClockMapping {
  std::string from_domain;
  std::string to_domain;
  TimeNs offset_ns = 0;
  // Half-width of the measurement interval. Unknown stays unknown.
  std::optional<TimeNs> uncertainty_ns;
  std::string method;  // how it was measured
  bool measured = false;

  json::Value to_json() const;
};

// Per-event data-quality markers. These travel with the event into every
// derived metric and issue so no conclusion can outrun its evidence.
enum class QualityFlag {
  kIncompleteSpan,       // begin or end missing (spec D11)
  kOutOfOrder,           // arrived out of monotonic order (D10)
  kDuplicate,            // duplicate event_id seen (D10)
  kClockUnmapped,        // timestamp in a domain with no mapping (D12)
  kSyntheticFixture,     // came from a labelled synthetic fixture
  kProviderDropped,      // provider reported buffer loss around here (D09)
  kEstimatedDuration,    // duration derived, not measured
  kAmbiguousOwnership,   // process ownership not established (B07)
};
const char* to_string(QualityFlag f);

enum class EventCategory {
  kFrame,        // frame production / presentation evidence
  kJsExecution,  // JS task / Hermes span
  kCpuSample,    // sampled stack
  kSchedule,     // running / runnable / blocked
  kMemory,       // counter sample
  kIo,
  kNetwork,
  kMarker,       // SDK marker: screen mount, navigation, interaction
  kLifecycle,    // process/thread start & exit
  kToolingActivity,
  kCounter,
  kOther,
};
const char* to_string(EventCategory c);
EventCategory event_category_from_string(const std::string& s);

struct Event {
  std::string event_id;
  std::string session_id;
  std::string provider;
  std::string provider_version;
  std::string clock_domain;
  TimeNs timestamp = 0;
  // Absent means the duration is genuinely unknown, not zero.
  std::optional<TimeNs> duration_ns;
  std::string device_id;
  std::string process_instance_id;
  std::string thread_instance_id;
  EventCategory category = EventCategory::kOther;
  std::string name;
  std::string correlation_id;
  std::string parent_id;
  json::Value payload = json::Value::object();
  std::vector<QualityFlag> quality_flags;

  bool has_flag(QualityFlag f) const;
  void add_flag(QualityFlag f);
  json::Value to_json() const;
};

struct ThreadInfo {
  std::string thread_instance_id;
  std::string process_instance_id;
  std::int32_t tid = 0;
  std::string name;
  bool is_main_ui_thread = false;
  bool is_js_thread = false;
  std::optional<TimeNs> start_ns;
  std::optional<TimeNs> end_ns;

  json::Value to_json() const;
};

// A window in which a collector produced nothing. Distinct from a window in
// which it measured zero (spec section 6, D09, E13).
struct CoverageGap {
  std::string collector;
  TimeNs start_ns = 0;
  TimeNs end_ns = 0;
  std::string reason;  // "buffer_overrun", "collector_crash", "not_started"
  std::optional<std::int64_t> dropped_event_count;

  TimeNs duration_ns() const { return end_ns - start_ns; }
  json::Value to_json() const;
};

// What a collector actually covered, so a rule can decide whether it has
// enough data to run at all instead of silently reporting "no issue".
struct Coverage {
  std::string collector;
  TimeNs window_start_ns = 0;
  TimeNs window_end_ns = 0;
  std::int64_t event_count = 0;
  std::vector<CoverageGap> gaps;

  TimeNs covered_ns() const;
  double covered_fraction() const;
  json::Value to_json() const;
};

}  // namespace mpi::model
