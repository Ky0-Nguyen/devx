#include "adapters/browserstack/profiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/observe/observation_store.hpp"
#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/session/session_store.hpp"
#include "core/symbols/symbol_service.hpp"
#include "core/util/time.hpp"

namespace mpi::browserstack {
namespace {

namespace fs = std::filesystem;

double number(const json::Value& v) {
  if (v.is_number()) return v.as_double();
  if (v.is_string()) return std::atof(v.as_string().c_str());
  return std::nan("");
}

const json::Value* first_of(const json::Value& o, std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (const auto* v = o.find(k)) return v;
  }
  return nullptr;
}

// Seconds, milliseconds, microseconds or nanoseconds, by magnitude, to ns.
std::int64_t to_ns(double t) {
  const double a = std::fabs(t);
  if (a > 1e17) return static_cast<std::int64_t>(t);           // ns epoch
  if (a > 1e14) return static_cast<std::int64_t>(t * 1e3);     // us epoch
  if (a > 1e11) return static_cast<std::int64_t>(t * 1e6);     // ms epoch
  return static_cast<std::int64_t>(t * 1e9);                   // s, epoch or relative
}

// Every `*_data` link in the summary, with the metric it belongs to.
void collect_links(const json::Value& v, const std::string& path,
                   std::vector<std::pair<std::string, std::string>>& out) {
  if (v.is_object()) {
    for (const auto& [k, child] : v.members()) {
      const std::string here = path.empty() ? k : path + "." + k;
      if (child.is_string() && k.size() > 5 && k.compare(k.size() - 5, 5, "_data") == 0 &&
          child.as_string().rfind("https://", 0) == 0) {
        out.emplace_back(here, child.as_string());
      } else {
        collect_links(child, here, out);
      }
    }
  } else if (v.is_array()) {
    for (const auto& child : v.items()) collect_links(child, path, out);
  }
}

// Name, unit and family of the DevX counter a metric's field becomes. The
// fields are BrowserStack's, measured on a Pixel 9 (Android 16) session; the
// units are those its summary states (`data.units`).
struct Mapping {
  std::string name, unit, family;
  double scale = 1.0;
};

Mapping map_field(const std::string& path, const std::string& field) {
  static const std::map<std::string, Mapping> kFields = {
      {"cpu_usage", {"cpu.usage_percent", "percent", "browserstack_cpu", 1}},
      {"cpu_threads", {"process.threads", "count", "browserstack_cpu", 1}},
      {"mem_usage", {"memory.used_bytes", "bytes", "browserstack_memory", 1024.0 * 1024.0}},
      {"batt_usage", {"battery.used_mah", "mAh", "browserstack_battery", 1}},
      {"reads", {"disk.read_kb", "kB", "browserstack_disk", 1}},
      {"writes", {"disk.write_kb", "kB", "browserstack_disk", 1}},
      {"uploads", {"network.upload_kb", "kB", "browserstack_network", 1}},
      {"downloads", {"network.download_kb", "kB", "browserstack_network", 1}},
      {"fps", {"ui.fps", "fps", "browserstack_fps", 1}},
      {"slow_fps", {"ui.slow_frames", "count", "browserstack_fps", 1}},
  };
  const auto it = kFields.find(field);
  if (it != kFields.end()) return it->second;
  // A field this reader does not know keeps its own name, unscaled.
  std::string tail = field;
  if (field == "value") {
    tail = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
  }
  return {"browserstack." + tail, "as reported", "browserstack_" + tail, 1};
}

std::string str(const json::Value& o, std::initializer_list<const char*> keys) {
  const auto* v = first_of(o, keys);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

}  // namespace

std::vector<Series> parse_series(const json::Value& doc, const std::string& text) {
  // Samples as (time, field, value), with whether the time is already an
  // offset from the session's start.
  struct Sample { double t; std::string field; double v; };
  std::vector<Sample> raw;
  bool offset_ms = false;
  const json::Value* arr = nullptr;
  if (doc.is_array()) {
    arr = &doc;
  } else if (doc.is_object()) {
    arr = first_of(doc, {"data", "values", "points", "series", "timeline"});
    if (arr != nullptr && !arr->is_array()) arr = nullptr;
  }
  if (arr != nullptr) {
    for (const auto& pt : arr->items()) {
      if (pt.is_array() && pt.items().size() >= 2) {
        raw.push_back({number(pt.items()[0]), "value", number(pt.items()[1])});
      } else if (pt.is_object()) {
        const char* tkey = nullptr;
        if (pt.find("time_offset_ms") != nullptr) {
          tkey = "time_offset_ms";
          offset_ms = true;
        } else {
          for (const char* k : {"t", "ts", "timestamp", "time", "epoch", "x"}) {
            if (pt.find(k) != nullptr) { tkey = k; break; }
          }
        }
        if (tkey == nullptr) continue;
        const double t = number(*pt.find(tkey));
        for (const auto& [k, v] : pt.members()) {
          if (k == tkey || !v.is_number()) continue;  // a null is no sample
          raw.push_back({t, k, v.as_double()});
        }
      }
    }
  } else if (!text.empty()) {
    // CSV: a header naming a time column; every other column is a series.
    std::istringstream in(text);
    std::string line;
    int tcol = -1;
    std::vector<std::string> names;
    bool header = true;
    while (std::getline(in, line)) {
      std::vector<std::string> cells;
      std::stringstream ls(line);
      std::string cell;
      while (std::getline(ls, cell, ',')) cells.push_back(cell);
      if (header) {
        names = cells;
        for (std::size_t i = 0; i < cells.size(); i++) {
          std::string h = cells[i];
          std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
          if (h.find("time") != std::string::npos || h == "t" || h == "ts") {
            tcol = static_cast<int>(i);
            offset_ms = h == "time_offset_ms";
            break;
          }
        }
        header = false;
        if (tcol < 0 || names.size() < 2) return {};
        continue;
      }
      if (static_cast<int>(cells.size()) <= tcol) continue;
      const double t = std::atof(cells[static_cast<std::size_t>(tcol)].c_str());
      for (std::size_t i = 0; i < cells.size() && i < names.size(); i++) {
        if (static_cast<int>(i) == tcol || cells[i].empty()) continue;
        char* endp = nullptr;
        const double v = std::strtod(cells[i].c_str(), &endp);
        if (endp != cells[i].c_str()) raw.push_back({t, names[i], v});
      }
    }
  }

  std::map<std::string, std::vector<std::pair<std::int64_t, double>>> by_field;
  std::int64_t t0 = INT64_MAX;
  for (const auto& s : raw) {
    if (std::isnan(s.t) || std::isnan(s.v)) continue;
    const std::int64_t ns = offset_ms ? static_cast<std::int64_t>(s.t * 1e6) : to_ns(s.t);
    t0 = std::min(t0, ns);
    by_field[s.field].emplace_back(ns, s.v);
  }
  std::vector<Series> out;
  for (auto& [field, pts] : by_field) {
    std::sort(pts.begin(), pts.end());
    // An offset from the session's start is kept, so series line up with one
    // another; anything else is counted from the first sample.
    if (!offset_ms) {
      for (auto& p : pts) p.first -= t0;
    }
    out.push_back({field, std::move(pts)});
  }
  return out;
}

json::Value ProfilingImport::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!devx_session_id.empty()) o.set("session_id", json::Value::string(devx_session_id));
  if (!package_dir.empty()) o.set("package", json::Value::string(package_dir));
  if (!observation_id.empty()) o.set("observation", json::Value::string(observation_id));
  o.set("series", json::Value::integer(series));
  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  o.set("notes", std::move(n));
  o.set("summary", summary);
  return o;
}

ProfilingImport import_profiling(const Credentials& c, const ProfilingRequest& req_in,
                                 const CancellationToken& cancel) {
  ProfilingImport out;
  ProfilingRequest req = req_in;
  auto id_ok = [](const std::string& s) {
    return !s.empty() && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
  };
  if (!id_ok(req.session_id)) {
    out.error = "not a BrowserStack session id: '" + req.session_id + "'";
    return out;
  }
  // The session's own details: its build, device and app, when not given.
  const auto details = get(c, "app-automate/sessions/" + req.session_id + ".json");
  if (details.ok) {
    const json::Value* s = details.body.find("automation_session");
    const json::Value& d = s != nullptr ? *s : details.body;
    if (req.build_id.empty()) req.build_id = str(d, {"build_hashed_id", "build_id"});
    if (req.device.empty()) req.device = str(d, {"device"});
    if (req.os.empty()) req.os = str(d, {"os"});
    if (req.os_version.empty()) req.os_version = str(d, {"os_version"});
    if (req.app.empty()) {
      if (const auto* a = d.find("app_details"); a != nullptr && a->is_object()) {
        req.app = str(*a, {"app_name", "app_url"});
      }
    }
  } else {
    out.notes.push_back("the session's details could not be read: " + details.error);
  }
  if (!id_ok(req.build_id)) {
    out.error = "the build of session " + req.session_id + " is not known; pass it";
    return out;
  }
  const auto prof = get(c, "app-automate/builds/" + req.build_id + "/sessions/" + req.session_id +
                               "/appprofiling/v2");
  if (!prof.ok) {
    out.error = "no profiling for this session: " + prof.error +
                ". It exists only for sessions run with appProfiling on, on a plan that "
                "includes it, and appears a little after the session ends";
    return out;
  }
  out.summary = prof.body;

  // The series behind each *_data link.
  std::vector<std::pair<std::string, std::string>> links;
  collect_links(prof.body, "", links);
  const std::string platform = req.os.find("ios") != std::string::npos ? "ios" : "android";
  const std::string device_id = "browserstack:" + req.session_id;
  const std::string app = req.app.empty() ? "unknown-app" : req.app;
  const std::string proc_id = platform + "|" + device_id + "|" + app + "|browserstack";
  json::Value counters = json::Value::array();
  std::int64_t end_ns = 0;
  json::Value raw_series = json::Value::object();
  std::set<std::string> made;
  std::int64_t start_ns = INT64_MAX;
  std::int64_t sample_count = 0;
  for (const auto& [metric, url] : links) {
    if (cancel.cancelled()) break;
    const auto r = request(c, "GET", url);
    if (!r.ok) {
      out.notes.push_back(metric + ": " + r.error);
      continue;
    }
    raw_series.set(metric, r.text.empty() ? r.body : json::Value::string(r.text.substr(0, 1 << 20)));
    const auto all = parse_series(r.body, r.text);
    if (all.empty()) {
      out.notes.push_back(metric + ": the data's shape was not recognised; it is kept raw");
      continue;
    }
    for (const auto& one : all) {
      const Mapping m = map_field(metric, one.field);
      // batt_usage rides along, mostly null, in every reply; the first
      // reply that carries a quantity is the one it is read from.
      if (!made.insert(m.name).second) continue;
      json::Value series = json::Value::object();
      series.set("name", json::Value::string(m.name));
      series.set("unit", json::Value::string(m.unit));
      series.set("provider", json::Value::string("BrowserStack App Profiling (" + metric + "." +
                                                 one.field + ")"));
      series.set("process_instance_id", json::Value::string(proc_id));
      series.set("family", json::Value::string(m.family));
      json::Value points = json::Value::array();
      for (const auto& [t, v] : one.points) {
        json::Value pt = json::Value::array();
        pt.push_back(json::Value::integer(t));
        pt.push_back(json::Value::number(v * m.scale));
        points.push_back(std::move(pt));
        end_ns = std::max(end_ns, t);
        start_ns = std::min(start_ns, t);
        sample_count++;
      }
      series.set("points", std::move(points));
      counters.push_back(std::move(series));
      out.series++;
    }
  }

  // The raw document, whatever happens to the conversion.
  {
    json::Value doc = json::Value::object();
    doc.set("build_id", json::Value::string(req.build_id));
    doc.set("session_id", json::Value::string(req.session_id));
    doc.set("summary", prof.body);
    doc.set("series", raw_series);
    json::Value sum = json::Value::object();
    sum.set("what", json::Value::string("app profiling of " + req.device));
    const auto saved = observe::save_observation(req.sessions_dir, "browserstack_profiling", app,
                                                 device_id, sum, doc);
    if (saved.ok) out.observation_id = saved.id;
  }

  // A trace in DevX's own format, read back through the ordinary reader.
  const std::string sid = session::new_session_id();
  json::Value t = json::Value::object();
  t.set("schema_version", json::Value::string("2.0"));
  t.set("session_id", json::Value::string(sid));
  json::Value dev = json::Value::object();
  dev.set("platform", json::Value::string(platform));
  dev.set("device_id", json::Value::string(device_id));
  dev.set("display_name", json::Value::string(req.device));
  dev.set("model", json::Value::string(req.device));
  dev.set("os_version", json::Value::string(req.os_version));
  dev.set("form", json::Value::string("physical"));
  dev.set("trust", json::Value::string("authorized"));
  dev.set("connection", json::Value::string("unknown"));
  dev.set("provider", json::Value::string("BrowserStack App Automate"));
  t.set("device", std::move(dev));
  json::Value key = json::Value::object();
  key.set("platform", json::Value::string(platform));
  key.set("device_id", json::Value::string(device_id));
  key.set("app_identifier", json::Value::string(app));
  key.set("identifier_kind", json::Value::string(platform == "ios" ? "bundle_id" : "package_name"));
  json::Value proc = json::Value::object();
  proc.set("application_key", key);
  proc.set("pid", json::Value::integer(0));
  proc.set("process_name", json::Value::string(app));
  proc.set("is_primary", json::Value::boolean(true));
  proc.set("ownership_evidence", json::Value::string("provider_attributed"));
  proc.set("ownership_note", json::Value::string("BrowserStack profiled the app it launched for this session"));
  proc.set("process_instance_id", json::Value::string(proc_id));
  json::Value procs = json::Value::array();
  procs.push_back(std::move(proc));
  json::Value target = json::Value::object();
  target.set("application_key", std::move(key));
  target.set("process_instances", std::move(procs));
  target.set("runtime_state", json::Value::string("running"));
  target.set("discovery_scope", json::Value::string("complete_for_provider"));
  t.set("target", std::move(target));
  json::Value domains = json::Value::array();
  json::Value dom = json::Value::object();
  dom.set("id", json::Value::string("browserstack.relative.ns"));
  dom.set("base", json::Value::string("monotonic"));
  dom.set("provider", json::Value::string(
      "BrowserStack App Profiling: time_offset_ms from the session's start"));
  dom.set("monotonic", json::Value::boolean(true));
  domains.push_back(std::move(dom));
  t.set("clock_domains", std::move(domains));
  t.set("primary_clock_domain", json::Value::string("browserstack.relative.ns"));
  t.set("window_start_ns", json::Value::integer(0));
  t.set("window_end_ns", json::Value::integer(end_ns));
  t.set("duration_ns", json::Value::integer(end_ns));
  t.set("counters", std::move(counters));
  // BrowserStack sampled from its first sample to its last; outside that,
  // nothing was measured, and the timeline must not say otherwise.
  json::Value coverage = json::Value::array();
  if (sample_count > 0 && end_ns > start_ns) {
    json::Value row = json::Value::object();
    row.set("collector", json::Value::string("browserstack.app_profiling"));
    row.set("window_start_ns", json::Value::integer(start_ns));
    row.set("window_end_ns", json::Value::integer(end_ns));
    row.set("event_count", json::Value::integer(sample_count));
    row.set("covered_ns", json::Value::integer(end_ns - start_ns));
    row.set("covered_fraction", json::Value::number(1));
    row.set("gaps", json::Value::array());
    coverage.push_back(std::move(row));
  }
  t.set("coverage", std::move(coverage));
  for (const char* k : {"events", "frames", "js_tasks", "markers", "cpu_samples", "threads"}) {
    t.set(k, json::Value::array());
  }
  json::Value warnings = json::Value::array();
  warnings.push_back(json::Value::string(
      "measured by BrowserStack App Profiling on their device, not by DevX; imported from build " +
      req.build_id + ", session " + req.session_id));
  for (const auto& n : out.notes) warnings.push_back(json::Value::string(n));
  t.set("ingestion_warnings", std::move(warnings));

  std::error_code ec;
  const std::string tmp = (fs::temp_directory_path(ec) / ("devx-bs-" + sid + ".mpi.json")).string();
  std::ofstream(tmp) << t.dump(1);
  model::NormalizedTrace trace;
  ingest::ReadOptions ropts;
  ropts.cancel = cancel;
  ingest::ReadDiagnostics diag;
  const auto reader = ingest::read_any(tmp, ropts, trace, diag);
  fs::remove(tmp, ec);
  if (!reader) {
    out.error = "the converted trace did not read back: " +
                (diag.errors.empty() ? std::string("unknown") : diag.errors.front());
    return out;
  }
  const auto norm = ingest::normalize(trace, cancel);
  for (const auto& n : norm.notes) trace.ingestion_warnings.push_back(n);
  symbols::SymbolService symbols;
  rules::EngineOptions eopts;
  eopts.cancel = cancel;
  auto analysis = rules::analyze(trace, symbols, eopts);
  analysis.data_quality_notes.push_back(
      "IMPORTED FROM BROWSERSTACK: these counters are BrowserStack's App Profiling of a real "
      "device in their cloud. DevX did not collect them, and a detector that needs frames, "
      "stacks or markers has none to work from");
  report::ReportOptions rep;
  const std::string md = report::to_markdown(trace, analysis, rep);
  const std::string js = report::to_json(trace, analysis, rep);
  session::SessionManifest manifest;
  manifest.session_id = sid;
  manifest.created_at = time_util::now_iso8601_utc();
  manifest.finalized_at = manifest.created_at;
  manifest.tool_version = rules::engine_version();
  manifest.state = session::SessionState::kCompleted;
  manifest.state_transitions.push_back("import from BrowserStack -> completed @ " + manifest.created_at);
  const auto written = session::write_package(req.sessions_dir, manifest, trace, analysis,
                                              model::DiscoverySnapshot{}, md, js);
  if (!written.ok) {
    out.error = "could not write the session: " + written.error;
    return out;
  }
  out.ok = true;
  out.devx_session_id = sid;
  out.package_dir = written.package_dir;
  return out;
}

}  // namespace mpi::browserstack
