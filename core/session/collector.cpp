#include "core/session/collector.hpp"
namespace mpi::session {
json::Value CaptureConfig::to_json() const {
  json::Value v = json::Value::object();
  v.set("duration_ms", json::Value::integer(duration.count()));
  v.set("sample_frequency_hz", json::Value::integer(sample_frequency_hz));
  v.set("frames", json::Value::boolean(frames));
  v.set("reset_frame_history", json::Value::boolean(reset_frame_history));
  v.set("cpu_samples", json::Value::boolean(cpu_samples));
  v.set("memory", json::Value::boolean(memory));
  v.set("memory_interval_ms", json::Value::integer(memory_interval.count()));
  v.set("preset", json::Value::string(preset));
  v.set("tick_interval_ms", json::Value::integer(tick_interval.count()));
  v.set("cpu_window_ms", json::Value::integer(cpu_window.count()));
  v.set("run_until_stopped", json::Value::boolean(run_until_stopped));
  return v;
}
json::Value LiveUpdate::to_json() const {
  json::Value v = json::Value::object();
  v.set("at_ns", json::Value::integer(at_ns));
  v.set("new_frames", json::Value::integer(new_frames));
  v.set("new_cpu_samples", json::Value::integer(new_cpu_samples));
  v.set("new_counter_points", json::Value::integer(new_counter_points));
  v.set("tick_cost_ms", json::Value::integer(tick_cost.count()));
  json::Value st = json::Value::array();
  for (const auto& c : source_status) st.push_back(c.to_json());
  v.set("source_status", std::move(st));
  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  v.set("notes", std::move(n));
  return v;
}

}  // namespace mpi::session
