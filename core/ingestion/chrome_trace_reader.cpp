#include <algorithm>
#include <fstream>
#include <map>
#include <vector>

#include "core/ingestion/reader.hpp"

namespace mpi::ingest {
namespace {

using model::TimeNs;

std::string sniff(const std::string& path, std::size_t n) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::string buf(n, '\0');
  f.read(buf.data(), static_cast<std::streamsize>(n));
  buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (!v) return {};
  if (v->is_string()) return v->as_string();
  if (v->is_number()) return std::to_string(v->as_int());
  return {};
}

// Chrome trace-event timestamps ("ts", "dur") are microseconds.
TimeNs us_to_ns(double us) { return static_cast<TimeNs>(us * 1000.0); }

// Thread and process ids in this format are integers, sometimes strings.
std::int64_t id_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (!v) return 0;
  if (v->is_number()) return v->as_int();
  if (v->is_string()) {
    // Accept a decimal string; anything else is 0 rather than a guess.
    try {
      return std::stoll(v->as_string());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// Heuristics used only to classify a thread, never to name a screen.
bool looks_like_js_thread(const std::string& name) {
  return name == "mqt_js" || name == "JavaScript" || name == "js" ||
         name.find("hermes") != std::string::npos ||
         name.find("JSThread") != std::string::npos;
}
bool looks_like_main_ui_thread(const std::string& name) {
  return name == "main" || name == "Main Thread" || name == "UI Thread" ||
         name == "CrRendererMain" || name == "com.apple.main-thread";
}

struct OpenSpan {
  std::string name;
  TimeNs start_ns;
  std::string category;
  std::string id;
};

}  // namespace

bool ChromeTraceReader::can_read(const std::string& path) const {
  const std::string head = sniff(path, 8192);
  if (head.find("\"traceEvents\"") != std::string::npos) return true;
  // Some exporters emit a bare array of events.
  const auto first = head.find_first_not_of(" \t\r\n");
  return first != std::string::npos && head[first] == '[' &&
         head.find("\"ph\"") != std::string::npos;
}

bool ChromeTraceReader::read(const std::string& path, const ReadOptions& opts,
                             model::NormalizedTrace& out, ReadDiagnostics& diag) {
  json::ParseError perr;
  auto parsed = json::parse_file(path, opts.json_limits, &perr);
  if (!parsed) {
    diag.errors.push_back("chrome trace parse failed at line " +
                          std::to_string(perr.line) + ": " + perr.message);
    return false;
  }

  const json::Value* events = nullptr;
  if (parsed->is_array()) {
    events = &*parsed;
  } else if (const json::Value* te = parsed->find("traceEvents");
             te && te->is_array()) {
    events = te;
  }
  if (!events) {
    diag.errors.push_back("no traceEvents array found");
    return false;
  }

  // This format carries no build state, no device identity, and no ownership
  // evidence. Everything it cannot answer stays unknown rather than defaulted.
  out.synthetic = out.synthetic || opts.mark_synthetic;
  if (out.primary_clock_domain.empty()) {
    out.primary_clock_domain = "chrome.trace_event.us";
  }
  model::ClockDomain cd;
  cd.id = out.primary_clock_domain;
  cd.base = "unknown";
  cd.provider = "chrome.trace_event.json";
  // The format does not state whether "ts" is monotonic. Saying "unknown" here
  // keeps every downstream duration honest.
  cd.monotonic = false;
  out.clock_domains.push_back(cd);
  diag.warnings.push_back(
      "chrome.trace_event.json does not declare its clock base; timestamps are "
      "treated as an unverified domain and cross-domain correlation is refused");

  std::map<std::int64_t, std::string> process_names;
  std::map<std::pair<std::int64_t, std::int64_t>, std::string> thread_names;

  // First pass: metadata events name the processes and threads.
  for (const auto& e : events->items()) {
    if (!e.is_object()) continue;
    if (str_at(e, "ph") != "M") continue;
    const std::string nm = str_at(e, "name");
    const json::Value* args = e.find("args");
    if (!args || !args->is_object()) continue;
    if (nm == "process_name") {
      process_names[id_at(e, "pid")] = str_at(*args, "name");
    } else if (nm == "thread_name") {
      thread_names[{id_at(e, "pid"), id_at(e, "tid")}] = str_at(*args, "name");
    }
  }

  auto process_instance_id = [&](std::int64_t pid) {
    // Without provider ownership evidence we can only key on the pid the file
    // states. Ownership stays unknown, which keeps such events out of
    // app-scoped totals unless the caller supplied a target.
    auto it = process_names.find(pid);
    const std::string nm = it == process_names.end() ? "unknown" : it->second;
    return "imported|pid=" + std::to_string(pid) + "|name=" + nm;
  };
  auto thread_instance_id = [&](std::int64_t pid, std::int64_t tid) {
    return process_instance_id(pid) + "|tid=" + std::to_string(tid);
  };

  for (const auto& kv : thread_names) {
    model::ThreadInfo ti;
    ti.process_instance_id = process_instance_id(kv.first.first);
    ti.thread_instance_id = thread_instance_id(kv.first.first, kv.first.second);
    ti.tid = static_cast<std::int32_t>(kv.first.second);
    ti.name = kv.second;
    ti.is_js_thread = looks_like_js_thread(kv.second);
    ti.is_main_ui_thread = looks_like_main_ui_thread(kv.second);
    out.threads.push_back(std::move(ti));
  }

  const bool keep_filter = !opts.keep_process_instance_ids.empty();
  auto kept = [&](const std::string& pid_key) {
    if (!keep_filter) return true;
    return std::find(opts.keep_process_instance_ids.begin(),
                     opts.keep_process_instance_ids.end(),
                     pid_key) != opts.keep_process_instance_ids.end();
  };

  // Open B events per (pid, tid, name) so an unmatched B stays incomplete
  // rather than being silently dropped or given a fabricated end (spec D11).
  std::map<std::string, std::vector<OpenSpan>> open_spans;
  std::int64_t index = 0;
  bool have_window = false;

  for (const auto& e : events->items()) {
    if (opts.cancel.cancelled()) {
      diag.cancelled = true;
      break;
    }
    if (!e.is_object()) {
      ++diag.events_rejected;
      continue;
    }
    const std::string ph = str_at(e, "ph");
    if (ph == "M") continue;  // handled above

    const json::Value* ts_v = e.find("ts");
    if (!ts_v || !ts_v->is_number()) {
      ++diag.events_rejected;
      continue;
    }
    const std::int64_t pid = id_at(e, "pid");
    const std::int64_t tid = id_at(e, "tid");
    const std::string pkey = process_instance_id(pid);
    if (!kept(pkey)) {
      ++diag.events_rejected;
      continue;
    }
    const std::string tkey = thread_instance_id(pid, tid);
    const TimeNs ts = us_to_ns(ts_v->as_double());
    const std::string name = str_at(e, "name");
    const std::string cat = str_at(e, "cat");

    if (!have_window) {
      out.window_start_ns = ts;
      out.window_end_ns = ts;
      have_window = true;
    } else {
      out.window_start_ns = std::min(out.window_start_ns, ts);
      out.window_end_ns = std::max(out.window_end_ns, ts);
    }

    model::Event ev;
    ev.event_id = "chrome-" + std::to_string(index++);
    ev.session_id = out.session_id;
    ev.provider = "chrome.trace_event.json";
    ev.clock_domain = out.primary_clock_domain;
    ev.timestamp = ts;
    ev.process_instance_id = pkey;
    ev.thread_instance_id = tkey;
    ev.name = name;
    ev.payload.set("cat", json::Value::string(cat));
    ev.payload.set("ph", json::Value::string(ph));
    if (out.synthetic) ev.add_flag(model::QualityFlag::kSyntheticFixture);
    // Ownership is not established by this format.
    ev.add_flag(model::QualityFlag::kAmbiguousOwnership);

    const model::ThreadInfo* ti = out.thread(tkey);
    const bool js_thread = ti && ti->is_js_thread;
    ev.category = js_thread ? model::EventCategory::kJsExecution
                            : model::EventCategory::kOther;

    if (ph == "X") {
      const json::Value* dur = e.find("dur");
      if (dur && dur->is_number()) {
        ev.duration_ns = us_to_ns(dur->as_double());
        out.window_end_ns = std::max(out.window_end_ns, ts + *ev.duration_ns);
      } else {
        // A complete event with no duration is incomplete, not instantaneous.
        ev.add_flag(model::QualityFlag::kIncompleteSpan);
      }
    } else if (ph == "B") {
      open_spans[tkey + "\x1f" + name].push_back(OpenSpan{name, ts, cat, ev.event_id});
      // The begin event alone is not yet a span; it is emitted when paired or
      // flagged incomplete at the end of the pass.
      ++diag.events_read;
      out.events.push_back(std::move(ev));
      continue;
    } else if (ph == "E") {
      auto it = open_spans.find(tkey + "\x1f" + name);
      if (it != open_spans.end() && !it->second.empty()) {
        const OpenSpan open = it->second.back();
        it->second.pop_back();
        // Attach the duration to the original B event.
        for (auto& prior : out.events) {
          if (prior.event_id == open.id) {
            prior.duration_ns = ts - open.start_ns;
            break;
          }
        }
        ++diag.events_read;
        continue;  // the E event itself is not a separate normalized event
      }
      // An E with no matching B: keep it, flagged, so the gap is visible.
      ev.add_flag(model::QualityFlag::kIncompleteSpan);
    } else if (ph == "C") {
      ev.category = model::EventCategory::kCounter;
      if (const json::Value* args = e.find("args"); args && args->is_object()) {
        ev.payload.set("args", *args);
      }
    } else if (ph == "i" || ph == "I" || ph == "R") {
      ev.duration_ns = std::nullopt;  // instant events genuinely have none
    }

    ++diag.events_read;
    out.events.push_back(std::move(ev));
  }

  // Anything still open never ended within the capture.
  std::int64_t unclosed = 0;
  for (const auto& kv : open_spans) {
    for (const auto& open : kv.second) {
      ++unclosed;
      for (auto& ev : out.events) {
        if (ev.event_id == open.id) {
          ev.add_flag(model::QualityFlag::kIncompleteSpan);
          break;
        }
      }
    }
  }
  if (unclosed > 0) {
    diag.warnings.push_back(std::to_string(unclosed) +
                            " span(s) had no matching end event and remain "
                            "incomplete");
    out.partial = true;
    out.partial_reasons.push_back("unterminated_spans_in_import");
  }

  // Derive JS tasks from JS-thread spans that carry a real duration.
  for (const auto& ev : out.events) {
    if (ev.category != model::EventCategory::kJsExecution) continue;
    if (!ev.duration_ns.has_value()) continue;
    if (ev.has_flag(model::QualityFlag::kIncompleteSpan)) continue;
    model::JsTask t;
    t.event_id = ev.event_id;
    t.start_ns = ev.timestamp;
    t.duration_ns = ev.duration_ns;
    t.name = ev.name;
    t.process_instance_id = ev.process_instance_id;
    t.thread_instance_id = ev.thread_instance_id;
    t.clock_domain = ev.clock_domain;
    // No measured mapping to a UI clock exists for an imported trace.
    t.clock_mapped_to_ui = false;
    if (out.synthetic) t.quality_flags.push_back(model::QualityFlag::kSyntheticFixture);
    out.js_tasks.push_back(std::move(t));
  }

  if (diag.events_read == 0) {
    diag.errors.push_back("traceEvents contained no usable events");
    return false;
  }
  out.ingestion_warnings.insert(out.ingestion_warnings.end(),
                                diag.warnings.begin(), diag.warnings.end());
  return true;
}

}  // namespace mpi::ingest
