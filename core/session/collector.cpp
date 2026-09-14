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
  return v;
}
}  // namespace mpi::session
