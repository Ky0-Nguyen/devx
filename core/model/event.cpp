#include "core/model/event.hpp"

#include <algorithm>

namespace mpi::model {

json::Value ClockDomain::to_json() const {
  json::Value v = json::Value::object();
  v.set("id", json::Value::string(id));
  v.set("base", json::Value::string(base));
  v.set("provider", json::Value::string(provider));
  v.set("monotonic", json::Value::boolean(monotonic));
  return v;
}

json::Value ClockMapping::to_json() const {
  json::Value v = json::Value::object();
  v.set("from_domain", json::Value::string(from_domain));
  v.set("to_domain", json::Value::string(to_domain));
  v.set("offset_ns", json::Value::integer(offset_ns));
  v.set("uncertainty_ns", uncertainty_ns.has_value()
                              ? json::Value::integer(*uncertainty_ns)
                              : json::Value::null());
  v.set("method", json::Value::string(method));
  v.set("measured", json::Value::boolean(measured));
  return v;
}

const char* to_string(QualityFlag f) {
  switch (f) {
    case QualityFlag::kIncompleteSpan: return "incomplete_span";
    case QualityFlag::kOutOfOrder: return "out_of_order";
    case QualityFlag::kDuplicate: return "duplicate";
    case QualityFlag::kClockUnmapped: return "clock_unmapped";
    case QualityFlag::kSyntheticFixture: return "synthetic_fixture";
    case QualityFlag::kProviderDropped: return "provider_dropped";
    case QualityFlag::kEstimatedDuration: return "estimated_duration";
    case QualityFlag::kAmbiguousOwnership: return "ambiguous_ownership";
  }
  return "unknown";
}

const char* to_string(EventCategory c) {
  switch (c) {
    case EventCategory::kFrame: return "frame";
    case EventCategory::kJsExecution: return "js_execution";
    case EventCategory::kCpuSample: return "cpu_sample";
    case EventCategory::kSchedule: return "schedule";
    case EventCategory::kMemory: return "memory";
    case EventCategory::kIo: return "io";
    case EventCategory::kNetwork: return "network";
    case EventCategory::kMarker: return "marker";
    case EventCategory::kLifecycle: return "lifecycle";
    case EventCategory::kToolingActivity: return "tooling_activity";
    case EventCategory::kCounter: return "counter";
    case EventCategory::kOther: return "other";
  }
  return "other";
}

EventCategory event_category_from_string(const std::string& s) {
  if (s == "frame") return EventCategory::kFrame;
  if (s == "js_execution") return EventCategory::kJsExecution;
  if (s == "cpu_sample") return EventCategory::kCpuSample;
  if (s == "schedule") return EventCategory::kSchedule;
  if (s == "memory") return EventCategory::kMemory;
  if (s == "io") return EventCategory::kIo;
  if (s == "network") return EventCategory::kNetwork;
  if (s == "marker") return EventCategory::kMarker;
  if (s == "lifecycle") return EventCategory::kLifecycle;
  if (s == "tooling_activity") return EventCategory::kToolingActivity;
  if (s == "counter") return EventCategory::kCounter;
  return EventCategory::kOther;
}

bool Event::has_flag(QualityFlag f) const {
  return std::find(quality_flags.begin(), quality_flags.end(), f) !=
         quality_flags.end();
}

void Event::add_flag(QualityFlag f) {
  if (!has_flag(f)) quality_flags.push_back(f);
}

json::Value Event::to_json() const {
  json::Value v = json::Value::object();
  v.set("event_id", json::Value::string(event_id));
  v.set("session_id", json::Value::string(session_id));
  v.set("provider", json::Value::string(provider));
  v.set("provider_version", json::Value::string(provider_version));
  v.set("clock_domain", json::Value::string(clock_domain));
  v.set("timestamp_ns", json::Value::integer(timestamp));
  // Absent duration stays null: unknown is not zero (spec section 6).
  v.set("duration_ns", duration_ns.has_value()
                           ? json::Value::integer(*duration_ns)
                           : json::Value::null());
  v.set("device_id", json::Value::string(device_id));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("thread_instance_id", json::Value::string(thread_instance_id));
  v.set("category", json::Value::string(to_string(category)));
  v.set("name", json::Value::string(name));
  v.set("correlation_id", correlation_id.empty()
                              ? json::Value::null()
                              : json::Value::string(correlation_id));
  v.set("parent_id",
        parent_id.empty() ? json::Value::null() : json::Value::string(parent_id));
  v.set("payload", payload);
  json::Value flags = json::Value::array();
  for (const auto f : quality_flags) flags.push_back(json::Value::string(to_string(f)));
  v.set("quality_flags", std::move(flags));
  return v;
}

json::Value ThreadInfo::to_json() const {
  json::Value v = json::Value::object();
  v.set("thread_instance_id", json::Value::string(thread_instance_id));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("tid", json::Value::integer(tid));
  v.set("name", json::Value::string(name));
  v.set("is_main_ui_thread", json::Value::boolean(is_main_ui_thread));
  v.set("is_js_thread", json::Value::boolean(is_js_thread));
  v.set("start_ns",
        start_ns.has_value() ? json::Value::integer(*start_ns) : json::Value::null());
  v.set("end_ns",
        end_ns.has_value() ? json::Value::integer(*end_ns) : json::Value::null());
  return v;
}

json::Value CoverageGap::to_json() const {
  json::Value v = json::Value::object();
  v.set("collector", json::Value::string(collector));
  v.set("start_ns", json::Value::integer(start_ns));
  v.set("end_ns", json::Value::integer(end_ns));
  v.set("duration_ns", json::Value::integer(duration_ns()));
  v.set("reason", json::Value::string(reason));
  v.set("dropped_event_count", dropped_event_count.has_value()
                                   ? json::Value::integer(*dropped_event_count)
                                   : json::Value::null());
  return v;
}

TimeNs Coverage::covered_ns() const {
  TimeNs total = window_end_ns - window_start_ns;
  if (total <= 0) return 0;
  TimeNs lost = 0;
  for (const auto& g : gaps) {
    const TimeNs s = std::max(g.start_ns, window_start_ns);
    const TimeNs e = std::min(g.end_ns, window_end_ns);
    if (e > s) lost += (e - s);
  }
  const TimeNs covered = total - lost;
  return covered > 0 ? covered : 0;
}

double Coverage::covered_fraction() const {
  const TimeNs total = window_end_ns - window_start_ns;
  if (total <= 0) return 0.0;
  return static_cast<double>(covered_ns()) / static_cast<double>(total);
}

json::Value Coverage::to_json() const {
  json::Value v = json::Value::object();
  v.set("collector", json::Value::string(collector));
  v.set("window_start_ns", json::Value::integer(window_start_ns));
  v.set("window_end_ns", json::Value::integer(window_end_ns));
  v.set("event_count", json::Value::integer(event_count));
  v.set("covered_ns", json::Value::integer(covered_ns()));
  v.set("covered_fraction", json::Value::number(covered_fraction()));
  json::Value g = json::Value::array();
  for (const auto& x : gaps) g.push_back(x.to_json());
  v.set("gaps", std::move(g));
  return v;
}

}  // namespace mpi::model
