#include "core/model/trace.hpp"

#include <algorithm>

namespace mpi::model {

std::optional<TimeNs> RefreshInterval::deadline_ns() const {
  // A variable-rate window has no single defensible deadline.
  if (variable || !hz.has_value() || *hz <= 0.0) return std::nullopt;
  return static_cast<TimeNs>(1e9 / *hz);
}

json::Value RefreshInterval::to_json() const {
  json::Value v = json::Value::object();
  v.set("start_ns", json::Value::integer(start_ns));
  v.set("end_ns", json::Value::integer(end_ns));
  v.set("hz", hz.has_value() ? json::Value::number(*hz) : json::Value::null());
  v.set("variable", json::Value::boolean(variable));
  const auto d = deadline_ns();
  v.set("deadline_ns", d.has_value() ? json::Value::integer(*d) : json::Value::null());
  v.set("provider", json::Value::string(provider));
  return v;
}

const char* to_string(FrameSource s) {
  switch (s) {
    case FrameSource::kPresentationTimestamps: return "presentation_timestamps";
    case FrameSource::kFrameDeadlineReports: return "frame_deadline_reports";
    case FrameSource::kDisplayCallbackProxy: return "display_callback_proxy";
    case FrameSource::kUnknown: return "unknown";
  }
  return "unknown";
}

std::optional<bool> FrameRecord::missed_deadline() const {
  if (!presented_ns.has_value() || !deadline_ns.has_value()) return std::nullopt;
  return (*presented_ns - start_ns) > *deadline_ns;
}

std::optional<TimeNs> FrameRecord::overrun_ns() const {
  if (!presented_ns.has_value() || !deadline_ns.has_value()) return std::nullopt;
  return (*presented_ns - start_ns) - *deadline_ns;
}

json::Value FrameRecord::to_json() const {
  json::Value v = json::Value::object();
  v.set("event_id", json::Value::string(event_id));
  v.set("start_ns", json::Value::integer(start_ns));
  v.set("presented_ns", presented_ns.has_value()
                            ? json::Value::integer(*presented_ns)
                            : json::Value::null());
  v.set("deadline_ns", deadline_ns.has_value() ? json::Value::integer(*deadline_ns)
                                               : json::Value::null());
  v.set("cpu_duration_ns", cpu_duration_ns.has_value()
                               ? json::Value::integer(*cpu_duration_ns)
                               : json::Value::null());
  v.set("source", json::Value::string(to_string(source)));
  // A proxy source is marked so no downstream text can call it presentation
  // truth (spec E21).
  v.set("is_presentation_truth",
        json::Value::boolean(source == FrameSource::kPresentationTimestamps));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("surface", surface.empty() ? json::Value::null() : json::Value::string(surface));
  const auto missed = missed_deadline();
  v.set("missed_deadline",
        missed.has_value() ? json::Value::boolean(*missed) : json::Value::null());
  const auto over = overrun_ns();
  v.set("overrun_ns", over.has_value() ? json::Value::integer(*over) : json::Value::null());
  json::Value fl = json::Value::array();
  for (const auto f : quality_flags) fl.push_back(json::Value::string(to_string(f)));
  v.set("quality_flags", std::move(fl));
  return v;
}

json::Value CpuSample::to_json() const {
  json::Value v = json::Value::object();
  v.set("timestamp_ns", json::Value::integer(timestamp_ns));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("thread_instance_id", json::Value::string(thread_instance_id));
  json::Value f = json::Value::array();
  for (const auto& s : frames) f.push_back(json::Value::string(s));
  v.set("frames", std::move(f));
  v.set("weight", weight.has_value() ? json::Value::number(*weight) : json::Value::null());
  v.set("provider", json::Value::string(provider));
  return v;
}

json::Value JsTask::to_json() const {
  json::Value v = json::Value::object();
  v.set("event_id", json::Value::string(event_id));
  v.set("start_ns", json::Value::integer(start_ns));
  v.set("duration_ns", duration_ns.has_value() ? json::Value::integer(*duration_ns)
                                               : json::Value::null());
  v.set("name", json::Value::string(name));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("thread_instance_id", json::Value::string(thread_instance_id));
  v.set("clock_domain", json::Value::string(clock_domain));
  v.set("clock_mapped_to_ui", json::Value::boolean(clock_mapped_to_ui));
  json::Value fl = json::Value::array();
  for (const auto f : quality_flags) fl.push_back(json::Value::string(to_string(f)));
  v.set("quality_flags", std::move(fl));
  return v;
}

json::Value Marker::to_json() const {
  json::Value v = json::Value::object();
  v.set("event_id", json::Value::string(event_id));
  v.set("timestamp_ns", json::Value::integer(timestamp_ns));
  v.set("duration_ns", duration_ns.has_value() ? json::Value::integer(*duration_ns)
                                               : json::Value::null());
  v.set("kind", json::Value::string(kind));
  v.set("screen", screen.empty() ? json::Value::null() : json::Value::string(screen));
  v.set("interaction",
        interaction.empty() ? json::Value::null() : json::Value::string(interaction));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("payload", payload);
  if (!clock_domain.empty()) {
    v.set("clock_domain", json::Value::string(clock_domain));
  }
  return v;
}

json::Value CounterSeries::to_json() const {
  json::Value v = json::Value::object();
  v.set("name", json::Value::string(name));
  v.set("unit", json::Value::string(unit));
  v.set("provider", json::Value::string(provider));
  v.set("process_instance_id", json::Value::string(process_instance_id));
  v.set("family", json::Value::string(family));
  json::Value pts = json::Value::array();
  for (const auto& p : points) {
    json::Value pair = json::Value::array();
    pair.push_back(json::Value::integer(p.first));
    pair.push_back(json::Value::number(p.second));
    pts.push_back(std::move(pair));
  }
  v.set("points", std::move(pts));
  return v;
}

json::Value TargetSelection::to_json() const {
  json::Value v = json::Value::object();
  v.set("application_key", app.to_json());
  json::Value p = json::Value::array();
  for (const auto& x : processes) p.push_back(x.to_json());
  v.set("process_instances", std::move(p));
  v.set("runtime_state", json::Value::string(to_string(runtime_state_at_capture)));
  v.set("discovery_scope", json::Value::string(to_string(discovery_scope)));
  return v;
}

const ThreadInfo* NormalizedTrace::thread(const std::string& id) const {
  for (const auto& t : threads) {
    if (t.thread_instance_id == id) return &t;
  }
  return nullptr;
}

const Coverage* NormalizedTrace::coverage_for(const std::string& collector) const {
  for (const auto& c : coverage) {
    if (c.collector == collector) return &c;
  }
  return nullptr;
}

std::optional<TimeNs> NormalizedTrace::map_to_primary(const std::string& domain,
                                                      TimeNs t) const {
  if (domain == primary_clock_domain) return t;
  for (const auto& m : clock_mappings) {
    if (m.from_domain == domain && m.to_domain == primary_clock_domain) {
      // Only a measured mapping may be applied. An assumed offset would make
      // a correlation look real when it is not (spec section 6).
      if (!m.measured) return std::nullopt;
      return t + m.offset_ns;
    }
  }
  return std::nullopt;
}

const RefreshInterval* NormalizedTrace::refresh_at(TimeNs t) const {
  for (const auto& r : refresh_intervals) {
    if (t >= r.start_ns && t < r.end_ns) return &r;
  }
  return nullptr;
}

json::Value NormalizedTrace::to_json(bool include_events) const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string(schema_version));
  v.set("session_id", json::Value::string(session_id));
  v.set("synthetic", json::Value::boolean(synthetic));
  v.set("synthetic_note", synthetic_note.empty() ? json::Value::null()
                                                 : json::Value::string(synthetic_note));
  v.set("device", device.to_json());
  v.set("target", target.to_json());
  v.set("build", build.to_json());
  v.set("capabilities", capabilities.to_json());
  v.set("requested_measurement_mode", json::Value::string(to_string(requested_mode)));

  json::Value cds = json::Value::array();
  for (const auto& c : clock_domains) cds.push_back(c.to_json());
  v.set("clock_domains", std::move(cds));
  json::Value cms = json::Value::array();
  for (const auto& c : clock_mappings) cms.push_back(c.to_json());
  v.set("clock_mappings", std::move(cms));
  v.set("primary_clock_domain", json::Value::string(primary_clock_domain));

  v.set("window_start_ns", json::Value::integer(window_start_ns));
  v.set("window_end_ns", json::Value::integer(window_end_ns));
  v.set("duration_ns", json::Value::integer(duration_ns()));

  json::Value th = json::Value::array();
  {
    // Samples per thread, counted once and attached to each thread.
    //
    // A report carries the thread list but not the samples -- they are in the
    // raw trace, which for a large capture is gigabytes. Without a count
    // here, a reader of the report can see which threads existed and nothing
    // about which one did the work, so any per-thread view has to re-read the
    // whole trace to say anything. One integer per thread is bounded by the
    // thread count, not by the capture's size.
    std::map<std::string, std::int64_t> per_thread;
    for (const auto& s : cpu_samples) {
      if (s.thread_instance_id.empty()) continue;
      ++per_thread[s.thread_instance_id];
    }
    for (const auto& t : threads) {
      json::Value entry = t.to_json();
      const auto it = per_thread.find(t.thread_instance_id);
      // Null when this capture collected no samples at all: a thread with no
      // samples attributed is not a thread that used no CPU, and 0 would read
      // as the second.
      entry.set("sample_count",
                cpu_samples.empty()
                    ? json::Value::null()
                    : json::Value::integer(it == per_thread.end() ? 0
                                                                  : it->second));
      th.push_back(std::move(entry));
    }
  }
  v.set("threads", std::move(th));

  json::Value counts = json::Value::object();
  counts.set("events", json::Value::integer(static_cast<std::int64_t>(events.size())));
  counts.set("frames", json::Value::integer(static_cast<std::int64_t>(frames.size())));
  counts.set("cpu_samples",
             json::Value::integer(static_cast<std::int64_t>(cpu_samples.size())));
  counts.set("js_tasks", json::Value::integer(static_cast<std::int64_t>(js_tasks.size())));
  counts.set("markers", json::Value::integer(static_cast<std::int64_t>(markers.size())));
  counts.set("counters", json::Value::integer(static_cast<std::int64_t>(counters.size())));
  v.set("counts", std::move(counts));

  json::Value ri = json::Value::array();
  for (const auto& r : refresh_intervals) ri.push_back(r.to_json());
  v.set("refresh_intervals", std::move(ri));

  json::Value cov = json::Value::array();
  for (const auto& c : coverage) cov.push_back(c.to_json());
  v.set("coverage", std::move(cov));

  json::Value drops = json::Value::object();
  for (const auto& kv : dropped_events_by_collector) {
    drops.set(kv.first, json::Value::integer(kv.second));
  }
  v.set("dropped_events_by_collector", std::move(drops));

  json::Value warn = json::Value::array();
  for (const auto& w : ingestion_warnings) warn.push_back(json::Value::string(w));
  v.set("ingestion_warnings", std::move(warn));
  v.set("partial", json::Value::boolean(partial));
  json::Value pr = json::Value::array();
  for (const auto& r : partial_reasons) pr.push_back(json::Value::string(r));
  v.set("partial_reasons", std::move(pr));

  if (include_events) {
    json::Value ev = json::Value::array();
    for (const auto& e : events) ev.push_back(e.to_json());
    v.set("events", std::move(ev));
    json::Value fr = json::Value::array();
    for (const auto& f : frames) fr.push_back(f.to_json());
    v.set("frames", std::move(fr));
    json::Value js = json::Value::array();
    for (const auto& j : js_tasks) js.push_back(j.to_json());
    v.set("js_tasks", std::move(js));
    json::Value mk = json::Value::array();
    for (const auto& m : markers) mk.push_back(m.to_json());
    v.set("markers", std::move(mk));
    json::Value cs = json::Value::array();
    for (const auto& c : cpu_samples) cs.push_back(c.to_json());
    v.set("cpu_samples", std::move(cs));
    json::Value ct = json::Value::array();
    for (const auto& c : counters) ct.push_back(c.to_json());
    v.set("counters", std::move(ct));
  }
  return v;
}

}  // namespace mpi::model
