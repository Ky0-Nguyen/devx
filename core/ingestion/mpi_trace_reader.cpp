#include <fstream>

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

std::optional<TimeNs> opt_int(const json::Value* v) {
  if (!v || v->is_null() || !v->is_number()) return std::nullopt;
  return v->as_int();
}

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return (v && v->is_string()) ? v->as_string() : std::string();
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
  json::ParseError perr;
  auto parsed = json::parse_file(path, opts.json_limits, &perr);
  if (!parsed) {
    diag.errors.push_back("json parse failed at line " +
                          std::to_string(perr.line) + ": " + perr.message);
    return false;
  }
  const json::Value& root_doc = *parsed;
  // Accept either a bare trace or a wrapper with a "normalized_trace" member.
  const json::Value* wrapped = root_doc.find("normalized_trace");
  const json::Value& root = wrapped ? *wrapped : root_doc;

  out.session_id = str_at(root, "session_id");
  out.schema_version = str_at(root, "schema_version");
  if (out.schema_version.empty()) out.schema_version = "unknown";
  if (out.schema_version != "2.0" && out.schema_version != "unknown") {
    // Spec D18: an unsupported schema is stated, not guessed at.
    diag.warnings.push_back("trace schema_version '" + out.schema_version +
                            "' is not 2.0; fields may be missing");
  }
  const json::Value* syn = root.find("synthetic");
  out.synthetic = (syn && syn->as_bool()) || opts.mark_synthetic;
  out.synthetic_note = str_at(root, "synthetic_note");
  if (opts.mark_synthetic && out.synthetic_note.empty()) {
    out.synthetic_note = "marked synthetic by the importing caller";
  }

  if (const json::Value* d = root.find("device"); d && d->is_object()) {
    read_device(*d, out.device);
  }

  if (const json::Value* t = root.find("target"); t && t->is_object()) {
    if (const json::Value* ak = t->find("application_key"); ak && ak->is_object()) {
      read_app_key(*ak, out.target.app);
    }
    out.target.runtime_state_at_capture = runtime_from_string(str_at(*t, "runtime_state"));
    out.target.discovery_scope = scope_from_string(str_at(*t, "discovery_scope"));
    if (const json::Value* ps = t->find("process_instances"); ps && ps->is_array()) {
      for (const auto& p : ps->items()) {
        if (!p.is_object()) continue;
        model::ProcessInstance pi;
        if (const json::Value* pk = p.find("application_key"); pk && pk->is_object()) {
          read_app_key(*pk, pi.app);
        } else {
          pi.app = out.target.app;
        }
        pi.pid = static_cast<std::int32_t>(p.find("pid") ? p.find("pid")->as_int() : 0);
        pi.process_start_time = str_at(p, "process_start_time");
        pi.process_name = str_at(p, "process_name");
        if (const json::Value* u = p.find("uid"); u && u->is_number()) {
          pi.uid = static_cast<std::int32_t>(u->as_int());
        }
        const json::Value* prim = p.find("is_primary");
        pi.is_primary = prim && prim->as_bool();
        pi.ownership = ownership_from_string(str_at(p, "ownership_evidence"));
        pi.ownership_note = str_at(p, "ownership_note");
        out.target.processes.push_back(std::move(pi));
      }
    }
  }

  if (const json::Value* b = root.find("build"); b && b->is_object()) {
    if (const json::Value* fs = b->find("facts"); fs && fs->is_array()) {
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
    if (const json::Value* sb = b->find("symbol_bindings"); sb && sb->is_array()) {
      for (const auto& s : sb->items()) {
        if (!s.is_object()) continue;
        model::SymbolBinding x;
        x.status = str_at(s, "status");
        x.kind = str_at(s, "kind");
        x.expected_id = str_at(s, "expected_id");
        x.actual_id = str_at(s, "actual_id");
        x.artifact_path = str_at(s, "artifact_path");
        x.note = str_at(s, "note");
        out.build.symbol_bindings.push_back(std::move(x));
      }
    }
  }

  out.requested_mode =
      model::measurement_mode_from_string(str_at(root, "requested_measurement_mode"));

  if (const json::Value* cds = root.find("clock_domains"); cds && cds->is_array()) {
    for (const auto& c : cds->items()) {
      if (!c.is_object()) continue;
      model::ClockDomain cd;
      cd.id = str_at(c, "id");
      cd.base = str_at(c, "base");
      cd.provider = str_at(c, "provider");
      const json::Value* m = c.find("monotonic");
      cd.monotonic = m && m->as_bool();
      out.clock_domains.push_back(std::move(cd));
    }
  }
  if (const json::Value* cms = root.find("clock_mappings"); cms && cms->is_array()) {
    for (const auto& c : cms->items()) {
      if (!c.is_object()) continue;
      model::ClockMapping cm;
      cm.from_domain = str_at(c, "from_domain");
      cm.to_domain = str_at(c, "to_domain");
      cm.offset_ns = c.find("offset_ns") ? c.find("offset_ns")->as_int() : 0;
      cm.uncertainty_ns = opt_int(c.find("uncertainty_ns"));
      cm.method = str_at(c, "method");
      const json::Value* meas = c.find("measured");
      cm.measured = meas && meas->as_bool();
      if (!cm.measured) {
        diag.warnings.push_back("clock mapping " + cm.from_domain + "->" +
                                cm.to_domain +
                                " is not measured; it will not be applied");
      }
      out.clock_mappings.push_back(std::move(cm));
    }
  }
  out.primary_clock_domain = str_at(root, "primary_clock_domain");

  out.window_start_ns = root.find("window_start_ns")
                            ? root.find("window_start_ns")->as_int()
                            : 0;
  out.window_end_ns =
      root.find("window_end_ns") ? root.find("window_end_ns")->as_int() : 0;

  if (const json::Value* ths = root.find("threads"); ths && ths->is_array()) {
    for (const auto& t : ths->items()) {
      if (!t.is_object()) continue;
      model::ThreadInfo ti;
      ti.thread_instance_id = str_at(t, "thread_instance_id");
      ti.process_instance_id = str_at(t, "process_instance_id");
      ti.tid = static_cast<std::int32_t>(t.find("tid") ? t.find("tid")->as_int() : 0);
      ti.name = str_at(t, "name");
      const json::Value* ui = t.find("is_main_ui_thread");
      ti.is_main_ui_thread = ui && ui->as_bool();
      const json::Value* js = t.find("is_js_thread");
      ti.is_js_thread = js && js->as_bool();
      ti.start_ns = opt_int(t.find("start_ns"));
      ti.end_ns = opt_int(t.find("end_ns"));
      out.threads.push_back(std::move(ti));
    }
  }

  const bool keep_filter = !opts.keep_process_instance_ids.empty();
  auto kept = [&](const std::string& pid) {
    if (!keep_filter) return true;
    for (const auto& k : opts.keep_process_instance_ids) {
      if (k == pid) return true;
    }
    return false;
  };

  if (const json::Value* evs = root.find("events"); evs && evs->is_array()) {
    for (const auto& e : evs->items()) {
      if (opts.cancel.cancelled()) {
        diag.cancelled = true;
        return diag.events_read > 0;
      }
      if (!e.is_object()) {
        ++diag.events_rejected;
        continue;
      }
      model::Event ev;
      ev.event_id = str_at(e, "event_id");
      ev.session_id = out.session_id;
      ev.provider = str_at(e, "provider");
      ev.provider_version = str_at(e, "provider_version");
      ev.clock_domain = str_at(e, "clock_domain");
      ev.timestamp = e.find("timestamp_ns") ? e.find("timestamp_ns")->as_int() : 0;
      ev.duration_ns = opt_int(e.find("duration_ns"));
      ev.device_id = str_at(e, "device_id");
      ev.process_instance_id = str_at(e, "process_instance_id");
      ev.thread_instance_id = str_at(e, "thread_instance_id");
      ev.category = model::event_category_from_string(str_at(e, "category"));
      ev.name = str_at(e, "name");
      ev.correlation_id = str_at(e, "correlation_id");
      ev.parent_id = str_at(e, "parent_id");
      if (const json::Value* p = e.find("payload"); p) ev.payload = *p;
      read_flags(e, ev.quality_flags, diag);
      if (out.synthetic) ev.add_flag(model::QualityFlag::kSyntheticFixture);
      if (!kept(ev.process_instance_id)) {
        ++diag.events_rejected;
        continue;
      }
      ++diag.events_read;
      out.events.push_back(std::move(ev));
    }
  }

  if (const json::Value* frs = root.find("frames"); frs && frs->is_array()) {
    for (const auto& f : frs->items()) {
      if (!f.is_object()) continue;
      model::FrameRecord fr;
      fr.event_id = str_at(f, "event_id");
      fr.start_ns = f.find("start_ns") ? f.find("start_ns")->as_int() : 0;
      fr.presented_ns = opt_int(f.find("presented_ns"));
      fr.deadline_ns = opt_int(f.find("deadline_ns"));
      fr.cpu_duration_ns = opt_int(f.find("cpu_duration_ns"));
      fr.source = frame_source_from_string(str_at(f, "source"));
      fr.process_instance_id = str_at(f, "process_instance_id");
      fr.surface = str_at(f, "surface");
      read_flags(f, fr.quality_flags, diag);
      if (out.synthetic) fr.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
      if (!kept(fr.process_instance_id)) continue;
      out.frames.push_back(std::move(fr));
    }
  }

  if (const json::Value* js = root.find("js_tasks"); js && js->is_array()) {
    for (const auto& j : js->items()) {
      if (!j.is_object()) continue;
      model::JsTask t;
      t.event_id = str_at(j, "event_id");
      t.start_ns = j.find("start_ns") ? j.find("start_ns")->as_int() : 0;
      t.duration_ns = opt_int(j.find("duration_ns"));
      t.name = str_at(j, "name");
      t.process_instance_id = str_at(j, "process_instance_id");
      t.thread_instance_id = str_at(j, "thread_instance_id");
      t.clock_domain = str_at(j, "clock_domain");
      const json::Value* cm = j.find("clock_mapped_to_ui");
      t.clock_mapped_to_ui = cm && cm->as_bool();
      read_flags(j, t.quality_flags, diag);
      if (out.synthetic) t.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
      if (!kept(t.process_instance_id)) continue;
      out.js_tasks.push_back(std::move(t));
    }
  }

  if (const json::Value* mks = root.find("markers"); mks && mks->is_array()) {
    for (const auto& m : mks->items()) {
      if (!m.is_object()) continue;
      model::Marker mk;
      mk.event_id = str_at(m, "event_id");
      mk.timestamp_ns = m.find("timestamp_ns") ? m.find("timestamp_ns")->as_int() : 0;
      mk.duration_ns = opt_int(m.find("duration_ns"));
      mk.kind = str_at(m, "kind");
      mk.screen = str_at(m, "screen");
      mk.interaction = str_at(m, "interaction");
      mk.process_instance_id = str_at(m, "process_instance_id");
      if (const json::Value* p = m.find("payload"); p) mk.payload = *p;
      if (!kept(mk.process_instance_id)) continue;
      out.markers.push_back(std::move(mk));
    }
  }

  if (const json::Value* cs = root.find("cpu_samples"); cs && cs->is_array()) {
    for (const auto& s : cs->items()) {
      if (!s.is_object()) continue;
      model::CpuSample smp;
      smp.timestamp_ns = s.find("timestamp_ns") ? s.find("timestamp_ns")->as_int() : 0;
      smp.process_instance_id = str_at(s, "process_instance_id");
      smp.thread_instance_id = str_at(s, "thread_instance_id");
      smp.provider = str_at(s, "provider");
      if (const json::Value* w = s.find("weight"); w && w->is_number()) {
        smp.weight = w->as_double();
      }
      if (const json::Value* fr = s.find("frames"); fr && fr->is_array()) {
        for (const auto& x : fr->items()) {
          if (x.is_string()) smp.frames.push_back(x.as_string());
        }
      }
      if (!kept(smp.process_instance_id)) continue;
      out.cpu_samples.push_back(std::move(smp));
    }
  }

  if (const json::Value* cts = root.find("counters"); cts && cts->is_array()) {
    for (const auto& c : cts->items()) {
      if (!c.is_object()) continue;
      model::CounterSeries s;
      s.name = str_at(c, "name");
      s.unit = str_at(c, "unit");
      s.provider = str_at(c, "provider");
      s.process_instance_id = str_at(c, "process_instance_id");
      s.family = str_at(c, "family");
      if (const json::Value* pts = c.find("points"); pts && pts->is_array()) {
        for (const auto& p : pts->items()) {
          if (!p.is_array() || p.items().size() < 2) continue;
          s.points.emplace_back(p.items()[0].as_int(), p.items()[1].as_double());
        }
      }
      if (!kept(s.process_instance_id)) continue;
      out.counters.push_back(std::move(s));
    }
  }

  if (const json::Value* ris = root.find("refresh_intervals"); ris && ris->is_array()) {
    for (const auto& r : ris->items()) {
      if (!r.is_object()) continue;
      model::RefreshInterval ri;
      ri.start_ns = r.find("start_ns") ? r.find("start_ns")->as_int() : 0;
      ri.end_ns = r.find("end_ns") ? r.find("end_ns")->as_int() : 0;
      if (const json::Value* h = r.find("hz"); h && h->is_number()) ri.hz = h->as_double();
      const json::Value* var = r.find("variable");
      ri.variable = var && var->as_bool();
      ri.provider = str_at(r, "provider");
      out.refresh_intervals.push_back(std::move(ri));
    }
  }

  if (const json::Value* cov = root.find("coverage"); cov && cov->is_array()) {
    for (const auto& c : cov->items()) {
      if (!c.is_object()) continue;
      model::Coverage cv;
      cv.collector = str_at(c, "collector");
      cv.window_start_ns = c.find("window_start_ns") ? c.find("window_start_ns")->as_int() : 0;
      cv.window_end_ns = c.find("window_end_ns") ? c.find("window_end_ns")->as_int() : 0;
      cv.event_count = c.find("event_count") ? c.find("event_count")->as_int() : 0;
      if (const json::Value* gs = c.find("gaps"); gs && gs->is_array()) {
        for (const auto& g : gs->items()) {
          if (!g.is_object()) continue;
          model::CoverageGap gap;
          gap.collector = cv.collector;
          gap.start_ns = g.find("start_ns") ? g.find("start_ns")->as_int() : 0;
          gap.end_ns = g.find("end_ns") ? g.find("end_ns")->as_int() : 0;
          gap.reason = str_at(g, "reason");
          gap.dropped_event_count = opt_int(g.find("dropped_event_count"));
          cv.gaps.push_back(std::move(gap));
        }
      }
      out.coverage.push_back(std::move(cv));
    }
  }

  if (const json::Value* dr = root.find("dropped_events_by_collector");
      dr && dr->is_object()) {
    for (const auto& kv : dr->members()) {
      out.dropped_events_by_collector[kv.first] = kv.second.as_int();
    }
  }

  const json::Value* part = root.find("partial");
  out.partial = part && part->as_bool();
  if (const json::Value* prs = root.find("partial_reasons"); prs && prs->is_array()) {
    for (const auto& p : prs->items()) {
      if (p.is_string()) out.partial_reasons.push_back(p.as_string());
    }
  }
  if (const json::Value* ws = root.find("ingestion_warnings"); ws && ws->is_array()) {
    for (const auto& w : ws->items()) {
      if (w.is_string()) out.ingestion_warnings.push_back(w.as_string());
    }
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
  // Order matters: the native format and the Hermes profile are both JSON
  // objects, and a Hermes profile also carries "traceEvents".
  Reader* readers[] = {&mpi_reader, &hermes, &chrome};
  for (Reader* r : readers) {
    if (!r->can_read(path)) continue;
    if (r->read(path, opts, out, diag)) return r->id();
    if (diag.fatal()) return std::nullopt;
  }
  if (diag.errors.empty()) {
    diag.errors.push_back("no reader recognised " + path +
                          " (supported: mpi.normalized.v2, "
                          "hermes.sampling_profile, chrome.trace_event.json)");
  }
  return std::nullopt;
}

}  // namespace mpi::ingest
