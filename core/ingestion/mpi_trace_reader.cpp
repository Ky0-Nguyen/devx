// Native session format reader.
//
// This reader streams. `json::parse()` would materialise the whole document,
// which costs roughly ten bytes of DOM per byte of input -- measured at 10.2 GB
// peak for a 1 GiB trace before this was changed. The bounded members (device,
// target, build, clocks) are still materialised because their size does not
// grow with capture length; the large arrays are walked one element at a time,
// so each element's DOM dies before the next is read.
#include <fstream>
#include <string>
#include <vector>

#include "core/ingestion/reader.hpp"
#include "core/util/time.hpp"

namespace mpi::ingest {
namespace {

using model::TimeNs;

std::string read_head(const std::string& path, std::size_t n) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::string buf(n, '\0');
  f.read(buf.data(), static_cast<std::streamsize>(n));
  buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

// Reads the file's bytes once. Held as one buffer; the saving comes from not
// building a DOM over it.
bool slurp(const std::string& path, std::size_t max_bytes, std::string& out,
           std::string& error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "cannot open file: " + path;
    return false;
  }
  const std::streamoff size = f.tellg();
  if (size < 0) {
    error = "cannot size file: " + path;
    return false;
  }
  if (static_cast<std::size_t>(size) > max_bytes) {
    error = "file exceeds max_bytes: " + path;
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  f.seekg(0);
  if (size > 0 && !f.read(out.data(), size)) {
    error = "read failed: " + path;
    return false;
  }
  return true;
}

std::optional<TimeNs> opt_int(const json::Value* v) {
  if (!v || v->is_null() || !v->is_number()) return std::nullopt;
  return v->as_int();
}

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return (v && v->is_string()) ? v->as_string() : std::string();
}

std::int64_t int_at(const json::Value& o, const char* key, std::int64_t fallback = 0) {
  const json::Value* v = o.find(key);
  return (v && v->is_number()) ? v->as_int() : fallback;
}

bool bool_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v && v->as_bool();
}

model::QualityFlag flag_from_string(const std::string& s, bool& ok) {
  ok = true;
  if (s == "incomplete_span") return model::QualityFlag::kIncompleteSpan;
  if (s == "out_of_order") return model::QualityFlag::kOutOfOrder;
  if (s == "duplicate") return model::QualityFlag::kDuplicate;
  if (s == "clock_unmapped") return model::QualityFlag::kClockUnmapped;
  if (s == "synthetic_fixture") return model::QualityFlag::kSyntheticFixture;
  if (s == "provider_dropped") return model::QualityFlag::kProviderDropped;
  if (s == "estimated_duration") return model::QualityFlag::kEstimatedDuration;
  if (s == "ambiguous_ownership") return model::QualityFlag::kAmbiguousOwnership;
  ok = false;
  return model::QualityFlag::kIncompleteSpan;
}

void read_flags(const json::Value& o, std::vector<model::QualityFlag>& out,
                ReadDiagnostics& diag) {
  const json::Value* arr = o.find("quality_flags");
  if (!arr || !arr->is_array()) return;
  for (const auto& f : arr->items()) {
    if (!f.is_string()) continue;
    bool ok = false;
    const auto v = flag_from_string(f.as_string(), ok);
    if (ok) {
      out.push_back(v);
    } else {
      diag.warnings.push_back("unknown quality_flag: " + f.as_string());
    }
  }
}

model::FrameSource frame_source_from_string(const std::string& s) {
  if (s == "presentation_timestamps")
    return model::FrameSource::kPresentationTimestamps;
  if (s == "frame_deadline_reports") return model::FrameSource::kFrameDeadlineReports;
  if (s == "display_callback_proxy") return model::FrameSource::kDisplayCallbackProxy;
  return model::FrameSource::kUnknown;
}

model::OwnershipEvidence ownership_from_string(const std::string& s) {
  if (s == "provider_attributed") return model::OwnershipEvidence::kProviderAttributed;
  if (s == "uid_and_process_name") return model::OwnershipEvidence::kUidAndProcessName;
  if (s == "ambiguous") return model::OwnershipEvidence::kAmbiguous;
  return model::OwnershipEvidence::kUnknown;
}

model::RuntimeState runtime_from_string(const std::string& s) {
  if (s == "running") return model::RuntimeState::kRunning;
  if (s == "not_running") return model::RuntimeState::kNotRunning;
  if (s == "suspended") return model::RuntimeState::kSuspended;
  return model::RuntimeState::kUnknown;
}

model::DiscoveryScope scope_from_string(const std::string& s) {
  if (s == "complete_for_provider") return model::DiscoveryScope::kCompleteForProvider;
  if (s == "partial") return model::DiscoveryScope::kPartial;
  return model::DiscoveryScope::kUnknown;
}

model::DeviceForm form_from_string(const std::string& s) {
  if (s == "physical") return model::DeviceForm::kPhysical;
  if (s == "simulator") return model::DeviceForm::kSimulator;
  if (s == "emulator") return model::DeviceForm::kEmulator;
  return model::DeviceForm::kUnknown;
}

model::TrustState trust_from_string(const std::string& s) {
  if (s == "authorized") return model::TrustState::kAuthorized;
  if (s == "unauthorized") return model::TrustState::kUnauthorized;
  if (s == "untrusted") return model::TrustState::kUntrusted;
  if (s == "locked") return model::TrustState::kLocked;
  if (s == "offline") return model::TrustState::kOffline;
  return model::TrustState::kUnknown;
}

model::Tri tri_from_json(const json::Value& o) {
  const json::Value* t = o.find("tri_state");
  if (t && t->is_string()) {
    if (t->as_string() == "true") return model::Tri::kTrue;
    if (t->as_string() == "false") return model::Tri::kFalse;
    return model::Tri::kUnknown;
  }
  const json::Value* b = o.find("boolean_value");
  if (b && b->is_bool()) return b->as_bool() ? model::Tri::kTrue : model::Tri::kFalse;
  return model::Tri::kUnknown;
}

model::FactSource fact_source_from_string(const std::string& s) {
  if (s == "build_plugin_manifest") return model::FactSource::kBuildPluginManifest;
  if (s == "runtime_sdk") return model::FactSource::kRuntimeSdk;
  if (s == "device_provider") return model::FactSource::kDeviceProvider;
  if (s == "host_toolchain") return model::FactSource::kHostToolchain;
  if (s == "user_asserted") return model::FactSource::kUserAsserted;
  if (s == "inferred") return model::FactSource::kInferred;
  return model::FactSource::kUnknown;
}

void read_device(const json::Value& o, model::DeviceRef& d) {
  d.platform = model::platform_from_string(str_at(o, "platform"));
  d.device_id = str_at(o, "device_id");
  d.display_name = str_at(o, "display_name");
  d.model = str_at(o, "model");
  d.os_version = str_at(o, "os_version");
  d.form = form_from_string(str_at(o, "form"));
  d.trust = trust_from_string(str_at(o, "trust"));
  d.provider = str_at(o, "provider");
  d.observed_at = str_at(o, "observed_at");
  d.boot_id = str_at(o, "boot_id");
}

void read_app_key(const json::Value& o, model::ApplicationKey& k) {
  k.platform = model::platform_from_string(str_at(o, "platform"));
  k.device_id = str_at(o, "device_id");
  k.app_identifier = str_at(o, "app_identifier");
  const std::string kind = str_at(o, "identifier_kind");
  k.identifier_kind = kind == "package_name" ? model::IdentifierKind::kPackageName
                      : kind == "bundle_id" ? model::IdentifierKind::kBundleId
                                            : model::IdentifierKind::kUnknown;
  const json::Value* u = o.find("android_user_id");
  if (u && u->is_number()) k.android_user_id = static_cast<int>(u->as_int());
}

// Decides whether an element belongs to the requested target. An empty filter
// keeps everything.
class KeepFilter {
 public:
  explicit KeepFilter(const std::vector<std::string>& ids) : ids_(ids) {}
  bool operator()(const std::string& process_instance_id) const {
    if (ids_.empty()) return true;
    for (const auto& k : ids_) {
      if (k == process_instance_id) return true;
    }
    return false;
  }

 private:
  const std::vector<std::string>& ids_;
};

// ---- per-element converters, each called once per streamed element ----------

bool convert_event(const json::Value& e, const model::NormalizedTrace& trace,
                   const KeepFilter& keep, ReadDiagnostics& diag,
                   model::Event& ev) {
  if (!e.is_object()) return false;
  ev = model::Event{};
  ev.event_id = str_at(e, "event_id");
  ev.session_id = trace.session_id;
  ev.provider = str_at(e, "provider");
  ev.provider_version = str_at(e, "provider_version");
  ev.clock_domain = str_at(e, "clock_domain");
  ev.timestamp = int_at(e, "timestamp_ns");
  ev.duration_ns = opt_int(e.find("duration_ns"));
  ev.device_id = str_at(e, "device_id");
  ev.process_instance_id = str_at(e, "process_instance_id");
  ev.thread_instance_id = str_at(e, "thread_instance_id");
  ev.category = model::event_category_from_string(str_at(e, "category"));
  ev.name = str_at(e, "name");
  ev.correlation_id = str_at(e, "correlation_id");
  ev.parent_id = str_at(e, "parent_id");
  if (const json::Value* pl = e.find("payload"); pl) ev.payload = *pl;
  read_flags(e, ev.quality_flags, diag);
  if (trace.synthetic) ev.add_flag(model::QualityFlag::kSyntheticFixture);
  return keep(ev.process_instance_id);
}

bool convert_frame(const json::Value& f, const model::NormalizedTrace& trace,
                   const KeepFilter& keep, ReadDiagnostics& diag,
                   model::FrameRecord& fr) {
  if (!f.is_object()) return false;
  fr = model::FrameRecord{};
  fr.event_id = str_at(f, "event_id");
  fr.start_ns = int_at(f, "start_ns");
  fr.presented_ns = opt_int(f.find("presented_ns"));
  fr.deadline_ns = opt_int(f.find("deadline_ns"));
  fr.cpu_duration_ns = opt_int(f.find("cpu_duration_ns"));
  fr.source = frame_source_from_string(str_at(f, "source"));
  fr.process_instance_id = str_at(f, "process_instance_id");
  fr.surface = str_at(f, "surface");
  read_flags(f, fr.quality_flags, diag);
  if (trace.synthetic) fr.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
  return keep(fr.process_instance_id);
}

bool convert_js_task(const json::Value& j, const model::NormalizedTrace& trace,
                     const KeepFilter& keep, ReadDiagnostics& diag,
                     model::JsTask& t) {
  if (!j.is_object()) return false;
  t = model::JsTask{};
  t.event_id = str_at(j, "event_id");
  t.start_ns = int_at(j, "start_ns");
  t.duration_ns = opt_int(j.find("duration_ns"));
  t.name = str_at(j, "name");
  t.process_instance_id = str_at(j, "process_instance_id");
  t.thread_instance_id = str_at(j, "thread_instance_id");
  t.clock_domain = str_at(j, "clock_domain");
  t.clock_mapped_to_ui = bool_at(j, "clock_mapped_to_ui");
  read_flags(j, t.quality_flags, diag);
  if (trace.synthetic) t.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
  return keep(t.process_instance_id);
}

bool convert_marker(const json::Value& m, const KeepFilter& keep,
                    model::Marker& mk) {
  if (!m.is_object()) return false;
  mk = model::Marker{};
  mk.event_id = str_at(m, "event_id");
  mk.timestamp_ns = int_at(m, "timestamp_ns");
  mk.duration_ns = opt_int(m.find("duration_ns"));
  mk.kind = str_at(m, "kind");
  mk.screen = str_at(m, "screen");
  mk.interaction = str_at(m, "interaction");
  mk.process_instance_id = str_at(m, "process_instance_id");
  if (const json::Value* pl = m.find("payload"); pl) mk.payload = *pl;
  return keep(mk.process_instance_id);
}

bool convert_cpu_sample(const json::Value& s, const KeepFilter& keep,
                        model::CpuSample& smp) {
  if (!s.is_object()) return false;
  smp = model::CpuSample{};
  smp.timestamp_ns = int_at(s, "timestamp_ns");
  smp.process_instance_id = str_at(s, "process_instance_id");
  smp.thread_instance_id = str_at(s, "thread_instance_id");
  smp.provider = str_at(s, "provider");
  if (const json::Value* w = s.find("weight"); w && w->is_number()) {
    smp.weight = w->as_double();
  }
  if (const json::Value* fr = s.find("frames"); fr && fr->is_array()) {
    smp.frames.reserve(fr->items().size());
    for (const auto& x : fr->items()) {
      if (x.is_string()) smp.frames.push_back(x.as_string());
    }
  }
  return keep(smp.process_instance_id);
}

bool convert_counter(const json::Value& c, const KeepFilter& keep,
                     model::CounterSeries& series) {
  if (!c.is_object()) return false;
  series = model::CounterSeries{};
  series.name = str_at(c, "name");
  series.unit = str_at(c, "unit");
  series.provider = str_at(c, "provider");
  series.process_instance_id = str_at(c, "process_instance_id");
  series.family = str_at(c, "family");
  if (const json::Value* pts = c.find("points"); pts && pts->is_array()) {
    series.points.reserve(pts->items().size());
    for (const auto& pt : pts->items()) {
      if (!pt.is_array() || pt.items().size() < 2) continue;
      series.points.emplace_back(pt.items()[0].as_int(), pt.items()[1].as_double());
    }
  }
  return keep(series.process_instance_id);
}

// ---- bounded members --------------------------------------------------------

void read_target(const json::Value& t, model::NormalizedTrace& out) {
  if (const json::Value* ak = t.find("application_key"); ak && ak->is_object()) {
    read_app_key(*ak, out.target.app);
  }
  out.target.runtime_state_at_capture = runtime_from_string(str_at(t, "runtime_state"));
  out.target.discovery_scope = scope_from_string(str_at(t, "discovery_scope"));
  if (const json::Value* ps = t.find("process_instances"); ps && ps->is_array()) {
    for (const auto& pv : ps->items()) {
      if (!pv.is_object()) continue;
      model::ProcessInstance pi;
      if (const json::Value* pk = pv.find("application_key"); pk && pk->is_object()) {
        read_app_key(*pk, pi.app);
      } else {
        pi.app = out.target.app;
      }
      pi.pid = static_cast<std::int32_t>(int_at(pv, "pid"));
      pi.process_start_time = str_at(pv, "process_start_time");
      pi.process_name = str_at(pv, "process_name");
      if (const json::Value* u = pv.find("uid"); u && u->is_number()) {
        pi.uid = static_cast<std::int32_t>(u->as_int());
      }
      pi.is_primary = bool_at(pv, "is_primary");
      pi.ownership = ownership_from_string(str_at(pv, "ownership_evidence"));
      pi.ownership_note = str_at(pv, "ownership_note");
      pi.boot_id = str_at(pv, "boot_id");
      out.target.processes.push_back(std::move(pi));
    }
  }
}

void read_build(const json::Value& b, model::NormalizedTrace& out) {
  if (const json::Value* fs = b.find("facts"); fs && fs->is_array()) {
    for (const auto& f : fs->items()) {
      if (!f.is_object()) continue;
      model::BuildFact bf;
      bf.key = str_at(f, "key");
      bf.value = str_at(f, "value");
      bf.boolean_value = tri_from_json(f);
      bf.source = fact_source_from_string(str_at(f, "source"));
      bf.observed_at = str_at(f, "observed_at");
      bf.basis = str_at(f, "basis");
      if (!bf.key.empty()) out.build.upsert(std::move(bf));
    }
  }
  if (const json::Value* sb = b.find("symbol_bindings"); sb && sb->is_array()) {
    for (const auto& sv : sb->items()) {
      if (!sv.is_object()) continue;
      model::SymbolBinding x;
      x.status = str_at(sv, "status");
      x.kind = str_at(sv, "kind");
      x.expected_id = str_at(sv, "expected_id");
      x.actual_id = str_at(sv, "actual_id");
      x.artifact_path = str_at(sv, "artifact_path");
      x.note = str_at(sv, "note");
      out.build.symbol_bindings.push_back(std::move(x));
    }
  }
}

void read_coverage(const json::Value& cov, model::NormalizedTrace& out) {
  if (!cov.is_array()) return;
  for (const auto& c : cov.items()) {
    if (!c.is_object()) continue;
    model::Coverage cv;
    cv.collector = str_at(c, "collector");
    cv.window_start_ns = int_at(c, "window_start_ns");
    cv.window_end_ns = int_at(c, "window_end_ns");
    cv.event_count = int_at(c, "event_count");
    if (const json::Value* gs = c.find("gaps"); gs && gs->is_array()) {
      for (const auto& g : gs->items()) {
        if (!g.is_object()) continue;
        model::CoverageGap gap;
        gap.collector = cv.collector;
        gap.start_ns = int_at(g, "start_ns");
        gap.end_ns = int_at(g, "end_ns");
        gap.reason = str_at(g, "reason");
        gap.dropped_event_count = opt_int(g.find("dropped_event_count"));
        cv.gaps.push_back(std::move(gap));
      }
    }
    out.coverage.push_back(std::move(cv));
  }
}

}  // namespace

bool MpiTraceReader::can_read(const std::string& path) const {
  const std::string head = read_head(path, 4096);
  return head.find("\"schema_version\"") != std::string::npos &&
         (head.find("\"primary_clock_domain\"") != std::string::npos ||
          head.find("\"normalized_trace\"") != std::string::npos ||
          head.find("\"target\"") != std::string::npos);
}

bool MpiTraceReader::read(const std::string& path, const ReadOptions& opts,
                          model::NormalizedTrace& out, ReadDiagnostics& diag) {
  std::string bytes;
  std::string slurp_error;
  if (!slurp(path, opts.json_limits.max_bytes, bytes, slurp_error)) {
    diag.errors.push_back(slurp_error);
    return false;
  }

  json::StreamParser sp(bytes, opts.json_limits);
  if (!sp.object_begin()) {
    diag.errors.push_back("json parse failed at line " +
                          std::to_string(sp.error().line) + ": " +
                          sp.error().message);
    return false;
  }

  // Some writers wrap the trace in {"normalized_trace": {...}}. Unwrap by
  // descending once, which the streaming parser supports because the wrapper's
  // only member is the trace itself.
  std::string key;
  bool unwrapped = false;
  if (!sp.next_member(key)) {
    diag.errors.push_back(sp.failed() ? sp.error().message
                                      : "trace object was empty");
    return false;
  }
  if (key == "normalized_trace") {
    if (!sp.object_begin() || !sp.next_member(key)) {
      diag.errors.push_back("malformed normalized_trace wrapper");
      return false;
    }
    unwrapped = true;
  }

  // `synthetic` may appear after the arrays, so the caller's override is
  // applied up front and the file's own flag folded in when it is seen. Events
  // read before the flag arrives are corrected in a final pass.
  out.synthetic = opts.mark_synthetic;
  if (opts.mark_synthetic) {
    out.synthetic_note = "marked synthetic by the importing caller";
  }
  bool synthetic_seen_late = false;
  std::size_t events_before_synthetic_flag = 0;

  const KeepFilter keep(opts.keep_process_instance_ids);

  // Reusable element scratch. Assigning into it each iteration releases the
  // previous element's storage, so peak DOM is one element.
  json::Value element;
  json::Value small;

  bool more = true;
  while (more) {
    if (opts.cancel.cancelled()) {
      diag.cancelled = true;
      break;
    }

    if (key == "events") {
      if (!sp.array_begin()) break;
      model::Event ev;
      while (sp.next_array_element(element)) {
        if (!convert_event(element, out, keep, diag, ev)) {
          ++diag.events_rejected;
          continue;
        }
        ++diag.events_read;
        out.events.push_back(std::move(ev));
        if (opts.cancel.cancelled()) {
          diag.cancelled = true;
          sp.array_skip_rest();
          break;
        }
      }
      if (!out.synthetic) events_before_synthetic_flag = out.events.size();
    } else if (key == "frames") {
      if (!sp.array_begin()) break;
      model::FrameRecord fr;
      while (sp.next_array_element(element)) {
        if (!convert_frame(element, out, keep, diag, fr)) continue;
        out.frames.push_back(std::move(fr));
      }
    } else if (key == "js_tasks") {
      if (!sp.array_begin()) break;
      model::JsTask t;
      while (sp.next_array_element(element)) {
        if (!convert_js_task(element, out, keep, diag, t)) continue;
        out.js_tasks.push_back(std::move(t));
      }
    } else if (key == "markers") {
      if (!sp.array_begin()) break;
      model::Marker mk;
      while (sp.next_array_element(element)) {
        if (!convert_marker(element, keep, mk)) continue;
        out.markers.push_back(std::move(mk));
      }
    } else if (key == "cpu_samples") {
      if (!sp.array_begin()) break;
      model::CpuSample smp;
      while (sp.next_array_element(element)) {
        if (!convert_cpu_sample(element, keep, smp)) continue;
        out.cpu_samples.push_back(std::move(smp));
      }
    } else if (key == "counters") {
      if (!sp.array_begin()) break;
      model::CounterSeries series;
      while (sp.next_array_element(element)) {
        if (!convert_counter(element, keep, series)) continue;
        out.counters.push_back(std::move(series));
      }
    } else if (key == "session_id") {
      if (!sp.read_value(small)) break;
      out.session_id = small.as_string();
    } else if (key == "schema_version") {
      if (!sp.read_value(small)) break;
      out.schema_version = small.as_string();
    } else if (key == "synthetic") {
      if (!sp.read_value(small)) break;
      if (small.as_bool()) {
        if (!out.synthetic && !out.events.empty()) synthetic_seen_late = true;
        out.synthetic = true;
      }
    } else if (key == "synthetic_note") {
      if (!sp.read_value(small)) break;
      if (!small.as_string().empty()) out.synthetic_note = small.as_string();
    } else if (key == "primary_clock_domain") {
      if (!sp.read_value(small)) break;
      out.primary_clock_domain = small.as_string();
    } else if (key == "requested_measurement_mode") {
      if (!sp.read_value(small)) break;
      out.requested_mode = model::measurement_mode_from_string(small.as_string());
    } else if (key == "window_start_ns") {
      if (!sp.read_value(small)) break;
      out.window_start_ns = small.as_int();
    } else if (key == "window_end_ns") {
      if (!sp.read_value(small)) break;
      out.window_end_ns = small.as_int();
    } else if (key == "partial") {
      if (!sp.read_value(small)) break;
      out.partial = small.as_bool();
    } else if (key == "device") {
      if (!sp.read_value(small)) break;
      if (small.is_object()) read_device(small, out.device);
    } else if (key == "target") {
      if (!sp.read_value(small)) break;
      if (small.is_object()) read_target(small, out);
    } else if (key == "build") {
      if (!sp.read_value(small)) break;
      if (small.is_object()) read_build(small, out);
    } else if (key == "coverage") {
      if (!sp.read_value(small)) break;
      read_coverage(small, out);
    } else if (key == "clock_domains") {
      if (!sp.read_value(small)) break;
      if (small.is_array()) {
        for (const auto& c : small.items()) {
          if (!c.is_object()) continue;
          model::ClockDomain cd;
          cd.id = str_at(c, "id");
          cd.base = str_at(c, "base");
          cd.provider = str_at(c, "provider");
          cd.monotonic = bool_at(c, "monotonic");
          out.clock_domains.push_back(std::move(cd));
        }
      }
    } else if (key == "clock_mappings") {
      if (!sp.read_value(small)) break;
      if (small.is_array()) {
        for (const auto& c : small.items()) {
          if (!c.is_object()) continue;
          model::ClockMapping cm;
          cm.from_domain = str_at(c, "from_domain");
          cm.to_domain = str_at(c, "to_domain");
          cm.offset_ns = int_at(c, "offset_ns");
          cm.uncertainty_ns = opt_int(c.find("uncertainty_ns"));
          cm.method = str_at(c, "method");
          cm.measured = bool_at(c, "measured");
          if (!cm.measured) {
            diag.warnings.push_back("clock mapping " + cm.from_domain + "->" +
                                    cm.to_domain +
                                    " is not measured; it will not be applied");
          }
          out.clock_mappings.push_back(std::move(cm));
        }
      }
    } else if (key == "threads") {
      if (!sp.read_value(small)) break;
      if (small.is_array()) {
        for (const auto& t : small.items()) {
          if (!t.is_object()) continue;
          model::ThreadInfo ti;
          ti.thread_instance_id = str_at(t, "thread_instance_id");
          ti.process_instance_id = str_at(t, "process_instance_id");
          ti.tid = static_cast<std::int32_t>(int_at(t, "tid"));
          ti.name = str_at(t, "name");
          ti.is_main_ui_thread = bool_at(t, "is_main_ui_thread");
          ti.is_js_thread = bool_at(t, "is_js_thread");
          ti.start_ns = opt_int(t.find("start_ns"));
          ti.end_ns = opt_int(t.find("end_ns"));
          out.threads.push_back(std::move(ti));
        }
      }
    } else if (key == "refresh_intervals") {
      if (!sp.read_value(small)) break;
      if (small.is_array()) {
        for (const auto& r : small.items()) {
          if (!r.is_object()) continue;
          model::RefreshInterval ri;
          ri.start_ns = int_at(r, "start_ns");
          ri.end_ns = int_at(r, "end_ns");
          if (const json::Value* h = r.find("hz"); h && h->is_number()) {
            ri.hz = h->as_double();
          }
          ri.variable = bool_at(r, "variable");
          ri.provider = str_at(r, "provider");
          out.refresh_intervals.push_back(std::move(ri));
        }
      }
    } else if (key == "dropped_events_by_collector") {
      if (!sp.read_value(small)) break;
      if (small.is_object()) {
        for (const auto& kv : small.members()) {
          out.dropped_events_by_collector[kv.first] = kv.second.as_int();
        }
      }
    } else if (key == "partial_reasons" || key == "ingestion_warnings") {
      if (!sp.read_value(small)) break;
      auto& sink = key == "partial_reasons" ? out.partial_reasons
                                            : out.ingestion_warnings;
      if (small.is_array()) {
        for (const auto& v : small.items()) {
          if (v.is_string()) sink.push_back(v.as_string());
        }
      }
    } else {
      // An unrecognised member is skipped without being materialised, so a
      // newer writer's extra fields cost nothing.
      if (!sp.skip_value()) break;
    }

    more = sp.next_member(key);
  }

  if (sp.failed()) {
    diag.errors.push_back("json parse failed at line " +
                          std::to_string(sp.error().line) + ": " +
                          sp.error().message);
    return false;
  }
  if (unwrapped) {
    // The wrapper's closing brace is not consumed; nothing else needs it.
  }

  if (out.schema_version.empty()) out.schema_version = "unknown";
  if (out.schema_version != "2.0" && out.schema_version != "unknown") {
    diag.warnings.push_back("trace schema_version '" + out.schema_version +
                            "' is not 2.0; fields may be missing");
  }

  // The `synthetic` flag can legitimately appear after the arrays it applies
  // to, so anything read before it is corrected here rather than left
  // unlabelled -- an unlabelled fixture event is exactly what spec H15 forbids.
  if (synthetic_seen_late) {
    for (std::size_t i = 0; i < events_before_synthetic_flag && i < out.events.size(); ++i) {
      out.events[i].add_flag(model::QualityFlag::kSyntheticFixture);
    }
    for (auto& f : out.frames) {
      bool has = false;
      for (const auto fl : f.quality_flags) {
        if (fl == model::QualityFlag::kSyntheticFixture) has = true;
      }
      if (!has) f.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
    }
    for (auto& j : out.js_tasks) {
      bool has = false;
      for (const auto fl : j.quality_flags) {
        if (fl == model::QualityFlag::kSyntheticFixture) has = true;
      }
      if (!has) j.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
    }
    diag.warnings.push_back(
        "the trace declared `synthetic` after its event arrays; the label was "
        "applied retroactively to every element read before it");
  }

  for (auto& ev : out.events) {
    if (ev.session_id.empty()) ev.session_id = out.session_id;
  }

  if (out.primary_clock_domain.empty() && !out.clock_domains.empty()) {
    out.primary_clock_domain = out.clock_domains.front().id;
    diag.warnings.push_back("primary_clock_domain absent; defaulted to '" +
                            out.primary_clock_domain + "'");
  }
  return true;
}

std::optional<std::string> read_any(const std::string& path,
                                    const ReadOptions& opts,
                                    model::NormalizedTrace& out,
                                    ReadDiagnostics& diag) {
  MpiTraceReader mpi_reader;
  HermesProfileReader hermes;
  ChromeTraceReader chrome;
  XctraceExportReader xctrace;
  // Order matters: the native format and the Hermes profile are both JSON
  // objects, and a Hermes profile also carries "traceEvents". The xctrace
  // export is XML, so it cannot be confused with any of them.
  Reader* readers[] = {&mpi_reader, &hermes, &chrome, &xctrace};
  for (Reader* r : readers) {
    if (!r->can_read(path)) continue;
    if (r->read(path, opts, out, diag)) return r->id();
    if (diag.fatal()) return std::nullopt;
  }
  if (diag.errors.empty()) {
    diag.errors.push_back("no reader recognised " + path +
                          " (supported: mpi.normalized.v2, "
                          "hermes.sampling_profile, chrome.trace_event.json, "
                          "ios.xctrace.export)");
  }
  return std::nullopt;
}

}  // namespace mpi::ingest
