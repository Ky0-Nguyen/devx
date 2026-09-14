#include <algorithm>
#include <fstream>
#include <map>
#include <string>
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

// The Hermes sampling profiler writes `ts` in microseconds.
TimeNs us_to_ns(double us) { return static_cast<TimeNs>(us * 1000.0); }

// One node of the profile's stack-frame tree.
struct FrameNode {
  std::string name;
  std::string category;
  std::string parent;
  std::string url;
  int line = -1;
  int column = -1;
};

// Walks parent links to produce an outermost-first frame list. Guards against
// a cyclic or self-referential parent chain in a malformed file (spec J02).
std::vector<std::string> unwind(const std::map<std::string, FrameNode>& nodes,
                                const std::string& leaf, std::size_t max_depth,
                                bool& truncated) {
  std::vector<std::string> inner_first;
  std::string cur = leaf;
  std::size_t guard = 0;
  while (!cur.empty() && guard++ < max_depth) {
    auto it = nodes.find(cur);
    if (it == nodes.end()) break;
    inner_first.push_back(it->second.name.empty() ? cur : it->second.name);
    if (it->second.parent == cur) break;  // self-referential
    cur = it->second.parent;
  }
  truncated = guard >= max_depth;
  std::reverse(inner_first.begin(), inner_first.end());
  return inner_first;
}

}  // namespace

bool HermesProfileReader::can_read(const std::string& path) const {
  const std::string head = sniff(path, 8192);
  // The distinguishing pair is stackFrames + samples. Chrome traces have
  // neither; a Chrome trace that happens to carry samples will still not have
  // a "stackFrames" object keyed by numeric frame ids.
  return head.find("\"stackFrames\"") != std::string::npos &&
         head.find("\"samples\"") != std::string::npos;
}

bool HermesProfileReader::read(const std::string& path, const ReadOptions& opts,
                               model::NormalizedTrace& out,
                               ReadDiagnostics& diag) {
  json::ParseError perr;
  auto parsed = json::parse_file(path, opts.json_limits, &perr);
  if (!parsed) {
    diag.errors.push_back("hermes profile parse failed at line " +
                          std::to_string(perr.line) + ": " + perr.message);
    return false;
  }

  const json::Value* frames_obj = parsed->find("stackFrames");
  const json::Value* samples_arr = parsed->find("samples");
  if (!frames_obj || !frames_obj->is_object() || !samples_arr ||
      !samples_arr->is_array()) {
    diag.errors.push_back(
        "not a Hermes sampling profile: stackFrames/samples missing or of the "
        "wrong type");
    return false;
  }

  out.synthetic = out.synthetic || opts.mark_synthetic;

  // JS samples live on the Hermes runtime's own clock. Spec section 11
  // requires an explicit mapping to the UI clock; none is present in the file,
  // so the domain stays separate and unmapped.
  const std::string js_domain = "js.hermes.us";
  if (out.primary_clock_domain.empty()) out.primary_clock_domain = js_domain;
  model::ClockDomain cd;
  cd.id = js_domain;
  cd.base = "unknown";
  cd.provider = "hermes.sampling_profile";
  cd.monotonic = false;
  out.clock_domains.push_back(cd);
  diag.warnings.push_back(
      "Hermes profile timestamps are on the JS runtime clock with no mapping "
      "to a UI/native clock in the file; no claim about UI impact can be "
      "derived from this input alone");

  std::map<std::string, FrameNode> nodes;
  for (const auto& kv : frames_obj->members()) {
    if (!kv.second.is_object()) continue;
    FrameNode n;
    n.name = str_at(kv.second, "name");
    n.category = str_at(kv.second, "category");
    n.parent = str_at(kv.second, "parent");
    // Hermes emits these when the bundle carries them; they are the only
    // legitimate basis for a source location before source-map resolution.
    n.url = str_at(kv.second, "funcVirtAddr");
    const json::Value* line = kv.second.find("line");
    const json::Value* col = kv.second.find("column");
    if (line && line->is_number()) n.line = static_cast<int>(line->as_int());
    if (col && col->is_number()) n.column = static_cast<int>(col->as_int());
    nodes[kv.first] = std::move(n);
  }
  if (nodes.empty()) {
    diag.errors.push_back("stackFrames was empty");
    return false;
  }

  // Hermes reports one logical process; thread ids appear per sample.
  const std::string pkey =
      out.target.processes.empty()
          ? "imported|hermes|pid=unknown"
          : out.target.processes.front().canonical();

  const bool keep_filter = !opts.keep_process_instance_ids.empty();
  if (keep_filter && std::find(opts.keep_process_instance_ids.begin(),
                               opts.keep_process_instance_ids.end(),
                               pkey) == opts.keep_process_instance_ids.end()) {
    diag.warnings.push_back(
        "hermes profile process key '" + pkey +
        "' is not in the requested target set; nothing was imported");
    return false;
  }

  std::map<std::string, model::ThreadInfo> threads;
  bool have_window = false;
  bool any_truncated_stack = false;
  std::int64_t index = 0;

  for (const auto& s : samples_arr->items()) {
    if (opts.cancel.cancelled()) {
      diag.cancelled = true;
      break;
    }
    if (!s.is_object()) {
      ++diag.events_rejected;
      continue;
    }
    const json::Value* ts_v = s.find("ts");
    const std::string sf = str_at(s, "sf");
    if (!ts_v || sf.empty()) {
      ++diag.events_rejected;
      continue;
    }
    // `ts` is a string in some Hermes versions and a number in others.
    double ts_us = 0.0;
    if (ts_v->is_number()) {
      ts_us = ts_v->as_double();
    } else if (ts_v->is_string()) {
      try {
        ts_us = std::stod(ts_v->as_string());
      } catch (...) {
        ++diag.events_rejected;
        continue;
      }
    } else {
      ++diag.events_rejected;
      continue;
    }
    const TimeNs ts = us_to_ns(ts_us);
    const std::string tid = str_at(s, "tid");
    const std::string tkey = pkey + "|tid=" + (tid.empty() ? "0" : tid);

    if (threads.find(tkey) == threads.end()) {
      model::ThreadInfo ti;
      ti.thread_instance_id = tkey;
      ti.process_instance_id = pkey;
      ti.name = str_at(s, "name");
      if (ti.name.empty()) ti.name = "hermes-js";
      ti.is_js_thread = true;
      threads[tkey] = ti;
    }

    if (!have_window) {
      out.window_start_ns = ts;
      out.window_end_ns = ts;
      have_window = true;
    } else {
      out.window_start_ns = std::min(out.window_start_ns, ts);
      out.window_end_ns = std::max(out.window_end_ns, ts);
    }

    bool truncated = false;
    model::CpuSample smp;
    smp.timestamp_ns = ts;
    smp.process_instance_id = pkey;
    smp.thread_instance_id = tkey;
    smp.provider = "hermes.sampling_profile";
    smp.frames = unwind(nodes, sf, /*max_depth=*/512, truncated);
    if (truncated) any_truncated_stack = true;
    if (smp.frames.empty()) {
      ++diag.events_rejected;
      continue;
    }
    out.cpu_samples.push_back(std::move(smp));

    model::Event ev;
    ev.event_id = "hermes-sample-" + std::to_string(index++);
    ev.session_id = out.session_id;
    ev.provider = "hermes.sampling_profile";
    ev.clock_domain = js_domain;
    ev.timestamp = ts;
    ev.process_instance_id = pkey;
    ev.thread_instance_id = tkey;
    ev.category = model::EventCategory::kCpuSample;
    ev.name = out.cpu_samples.back().frames.back();
    // A sample is a point observation; it has no duration of its own.
    ev.duration_ns = std::nullopt;
    if (js_domain != out.primary_clock_domain) {
      ev.add_flag(model::QualityFlag::kClockUnmapped);
    }
    if (out.synthetic) ev.add_flag(model::QualityFlag::kSyntheticFixture);
    ++diag.events_read;
    out.events.push_back(std::move(ev));
  }

  for (auto& kv : threads) out.threads.push_back(kv.second);

  if (any_truncated_stack) {
    diag.warnings.push_back(
        "one or more stacks hit the 512-frame unwind guard and were truncated");
  }

  // A Hermes profile may also carry traceEvents for its own metadata. Those
  // are not JS tasks and are deliberately not imported as such: a sampling
  // profile does not contain task boundaries, and inventing them would make
  // DET-02 report durations the provider never measured.
  if (const json::Value* te = parsed->find("traceEvents");
      te && te->is_array() && !te->items().empty()) {
    diag.warnings.push_back(
        "the profile's traceEvents section was not imported as JS tasks: a "
        "sampling profile has no task boundaries, so long-task duration "
        "cannot be derived from it");
  }

  if (out.cpu_samples.empty()) {
    diag.errors.push_back("no usable samples in profile");
    return false;
  }

  // Record what the sampler covered, including the sampling interval, so a
  // rule can refuse to name a function from too few samples (spec DET-04).
  model::Coverage cov;
  cov.collector = "hermes.sampling_profile";
  cov.window_start_ns = out.window_start_ns;
  cov.window_end_ns = out.window_end_ns;
  cov.event_count = static_cast<std::int64_t>(out.cpu_samples.size());
  out.coverage.push_back(std::move(cov));

  out.ingestion_warnings.insert(out.ingestion_warnings.end(),
                                diag.warnings.begin(), diag.warnings.end());
  return true;
}

}  // namespace mpi::ingest
