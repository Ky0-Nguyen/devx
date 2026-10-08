#include "core/correlation/engine.hpp"

#include "core/signals/redact.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace mpi::correlation {
namespace {

namespace fs = std::filesystem;
using signals::EvidenceBasis;
using signals::SignalRecord;

std::string jstr(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

std::optional<double> jnum(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  if (v == nullptr) return std::nullopt;
  if (v->is_number()) return v->as_double();
  if (v->is_string() && !v->as_string().empty()) {
    char* end = nullptr;
    const double d = std::strtod(v->as_string().c_str(), &end);
    if (end != v->as_string().c_str()) return d;
  }
  return std::nullopt;
}

std::optional<json::Value> read_json_file(const std::string& path, std::size_t max_bytes) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > max_bytes) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  std::string s(static_cast<std::size_t>(size), '\0');
  in.read(s.data(), static_cast<std::streamsize>(size));
  json::ParseError perr;
  return json::parse(s, json::Limits{}, &perr);
}

std::string release_node(const std::string& key) { return "release:" + key; }

bool is_ci(const SignalRecord& s) {
  return s.kind == "pipeline" || s.kind == "ci_job" || s.kind == "test";
}

bool is_production(const SignalRecord& s) {
  return s.kind == "crash" || s.kind == "issue" || s.kind == "metric" || s.kind == "event";
}

bool failed(const SignalRecord& s) {
  return s.severity && (*s.severity == "error" || *s.severity == "fatal");
}

const char* relation_for(const SignalRecord& s) {
  if (s.kind == "deploy") return "deployed_as";
  if (s.kind == "test") return "tested_in";
  if (is_ci(s)) return "built_from";
  return "observed_in";
}

// One SHA is the other, or one is a prefix of the other.
enum class ShaMatch { kNone, kFull, kPrefix };
ShaMatch sha_match(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return ShaMatch::kNone;
  if (a == b) return ShaMatch::kFull;
  const std::string& shorter = a.size() < b.size() ? a : b;
  const std::string& longer = a.size() < b.size() ? b : a;
  if (shorter.size() >= 7 && longer.rfind(shorter, 0) == 0) return ShaMatch::kPrefix;
  return ShaMatch::kNone;
}

std::string short_sha(const std::string& s) { return s.substr(0, 12); }

void widen(Release& r, const std::string& at) {
  if (at.empty()) return;
  if (r.first_activity_at.empty() || at < r.first_activity_at) r.first_activity_at = at;
  if (r.last_activity_at.empty() || at > r.last_activity_at) r.last_activity_at = at;
}

// Merges a member's identity into the release's, recording disagreements.
void merge_identity(Release& r, const signals::ReleaseIdentity& id, const std::string& who) {
  auto merge = [&](std::optional<std::string>& into, const std::optional<std::string>& from,
                   const char* field) {
    if (!from) return;
    if (!into) {
      into = from;
    } else if (*into != *from) {
      const std::string c = std::string(field) + ": '" + *into + "' and '" + *from + "' (" + who + ")";
      if (std::find(r.conflicts.begin(), r.conflicts.end(), c) == r.conflicts.end()) {
        r.conflicts.push_back(c);
      }
    }
  };
  merge(r.identity.version, id.version, "version");
  merge(r.identity.build_number, id.build_number, "build_number");
  merge(r.identity.commit_sha, id.commit_sha, "commit_sha");
  merge(r.identity.bundle_or_package_id, id.bundle_or_package_id, "bundle_or_package_id");
  if (!r.identity.branch && id.branch) r.identity.branch = id.branch;
  if (r.identity.source == signals::FactSource::kUnknown) r.identity.source = id.source;
}

// A compact entry for lists and overviews. Its text is redacted: a title or
// a root-error line is provider text, and these entries reach AI tools.
json::Value entry_json_raw(const SignalRecord& s);
json::Value entry_json(const SignalRecord& s) {
  return signals::redact_json(entry_json_raw(s), /*pii=*/true, /*ip_addresses=*/false, nullptr);
}

json::Value entry_json_raw(const SignalRecord& s) {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(s.id));
  o.set("provider", json::Value::string(s.provider));
  o.set("kind", json::Value::string(s.kind));
  if (s.severity) o.set("severity", json::Value::string(*s.severity));
  if (s.title) o.set("title", json::Value::string(s.title->substr(0, 200)));
  if (s.environment) o.set("environment", json::Value::string(*s.environment));
  o.set("occurred_at", json::Value::string(s.occurred_at));
  o.set("basis", json::Value::string(signals::to_string(s.basis)));
  // The few attributes a list needs, when present.
  json::Value a = json::Value::object();
  for (const char* k : {"count", "events", "user_count", "affected_installations", "status", "stage",
                        "job", "root_error", "p50", "p95", "unit", "samples", "failed", "total",
                        "event_name", "metric", "error_type"}) {
    if (const json::Value* v = s.attributes.find(k)) a.set(k, *v);
  }
  o.set("attributes", std::move(a));
  return o;
}

int write_private(const std::string& path, const std::string& bytes) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  const ssize_t n = ::write(fd, bytes.data(), bytes.size());
  ::close(fd);
  if (n != static_cast<ssize_t>(bytes.size())) return -1;
  return std::rename(tmp.c_str(), path.c_str());
}

// A metric's identity across releases: what it measures, not which build.
std::string metric_key(const SignalRecord& s) {
  std::string k = jstr(s.attributes, "event_type");
  const std::string name = jstr(s.attributes, "event_name");
  const std::string metric = jstr(s.attributes, "metric");
  k += "|" + (name.empty() ? metric : name);
  if (k == "|") k = s.title.value_or(s.external_id);
  return k;
}

}  // namespace

json::Value CorrelationEdge::to_json() const {
  json::Value o = json::Value::object();
  o.set("from", json::Value::string(from_id));
  o.set("to", json::Value::string(to_id));
  o.set("relation", json::Value::string(relation));
  o.set("basis", json::Value::string(signals::to_string(basis)));
  o.set("evidence", json::Value::string(evidence));
  if (source_ref) o.set("source_ref", json::Value::string(*source_ref));
  return o;
}

json::Value SessionRef::to_json() const {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(id));
  o.set("created_at", json::Value::string(created_at));
  o.set("platform", json::Value::string(platform));
  o.set("device_id", json::Value::string(device_id));
  o.set("app_identifier", json::Value::string(app_identifier));
  if (version) o.set("version", json::Value::string(*version));
  if (build) o.set("build", json::Value::string(*build));
  if (!version_source.empty()) o.set("version_source", json::Value::string(version_source));
  o.set("browserstack", json::Value::boolean(browserstack));
  return o;
}

std::vector<SessionRef> scan_sessions(const std::string& sessions_dir, std::size_t limit) {
  std::vector<std::string> ids;
  std::error_code ec;
  if (sessions_dir.empty() || !fs::is_directory(sessions_dir, ec)) return {};
  for (const auto& e : fs::directory_iterator(sessions_dir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("s-", 0) == 0 && e.is_directory(ec)) ids.push_back(name);
  }
  // Ids begin with their creation time, so a sort is newest-last.
  std::sort(ids.rbegin(), ids.rend());
  if (ids.size() > limit) ids.resize(limit);
  std::vector<SessionRef> out;
  for (const auto& id : ids) {
    const std::string dir = sessions_dir + "/" + id;
    SessionRef s;
    s.id = id;
    if (auto m = read_json_file(dir + "/manifest.json", 1 << 20)) s.created_at = jstr(*m, "created_at");
    if (auto c = read_json_file(dir + "/capture-config.json", 4 << 20)) {
      if (const json::Value* t = c->find("target")) {
        if (const json::Value* k = t->find("application_key")) {
          s.platform = jstr(*k, "platform");
          s.device_id = jstr(*k, "device_id");
          s.app_identifier = jstr(*k, "app_identifier");
        }
      }
    }
    s.browserstack = s.device_id.rfind("browserstack:", 0) == 0;
    if (auto b = read_json_file(dir + "/build.json", 4 << 20)) {
      std::map<std::string, std::string> facts;
      if (const json::Value* f = b->find("facts"); f != nullptr && f->is_array()) {
        for (const auto& fact : f->items()) {
          const std::string v = jstr(fact, "value");
          if (!v.empty()) facts[jstr(fact, "key")] = v;
        }
      }
      // The device's own record first (dumpsys package), then the SDK's.
      if (facts.count("app.version_name")) {
        s.version = facts["app.version_name"];
        if (facts.count("app.version_code")) s.build = facts["app.version_code"];
        s.version_source = "versionName / versionCode from the device";
      } else if (facts.count("app.version")) {
        s.version = facts["app.version"];
        if (facts.count("app.build_number")) s.build = facts["app.build_number"];
        s.version_source = "reported by the in-app SDK";
      }
    }
    if (s.app_identifier.empty()) continue;  // not a capture of an app
    out.push_back(std::move(s));
  }
  return out;
}

Graph build_graph(const Inputs& in) {
  Graph g;
  g.workspace_id = in.workspace.id;
  for (const auto& s : in.signals) g.signals[s.id] = s;

  // 1. Releases named by a version.
  std::vector<const SignalRecord*> commit_only;
  for (const auto& [id, s] : g.signals) {
    if (s.release.version) {
      const std::string key = s.release.key();
      Release& r = g.releases[key];
      r.key = key;
      merge_identity(r, s.release, s.provider + " " + s.kind + " " + s.external_id);
      if (s.kind != "release") r.signal_ids.push_back(id);
      widen(r, s.occurred_at);
      if (s.kind != "release") {
        CorrelationEdge e;
        e.from_id = id;
        e.to_id = release_node(key);
        e.relation = relation_for(s);
        e.basis = s.basis == EvidenceBasis::kExact ? EvidenceBasis::kExact : EvidenceBasis::kProviderAttributed;
        e.evidence = s.provider + " reports release " + key + " for this " + s.kind;
        e.source_ref = s.raw_ref.empty() ? std::nullopt : std::optional<std::string>(s.raw_ref);
        g.edges.push_back(std::move(e));
      }
    } else if (s.release.commit_sha) {
      commit_only.push_back(&s);
    }
  }

  // 2. Evidence that names only a commit (CI, deploys): the release that
  //    names the same commit, exactly; a prefix is only a candidate.
  for (const SignalRecord* s : commit_only) {
    const std::string sha = *s->release.commit_sha;
    bool exact = false;
    std::vector<CorrelationEdge> prefix_candidates;
    for (auto& [key, r] : g.releases) {
      if (key.rfind("commit:", 0) == 0 || !r.identity.commit_sha) continue;
      const ShaMatch m = sha_match(sha, *r.identity.commit_sha);
      if (m == ShaMatch::kNone) continue;
      CorrelationEdge e;
      e.from_id = s->id;
      e.to_id = release_node(key);
      e.relation = relation_for(*s);
      if (m == ShaMatch::kFull) {
        e.basis = EvidenceBasis::kExact;
        e.evidence = s->provider + " " + s->kind + " ran on commit " + short_sha(sha) +
                     ", the commit release " + key + " was built from";
        r.signal_ids.push_back(s->id);
        widen(r, s->occurred_at);
        if (s->kind == "deploy" && !failed(*s)) {
          const std::string at = jstr(s->attributes, "finished_at").empty() ? s->occurred_at
                                                                           : jstr(s->attributes, "finished_at");
          if (!r.deployed_at || at < *r.deployed_at) r.deployed_at = at;
        }
        g.edges.push_back(std::move(e));
        exact = true;
      } else {
        e.basis = EvidenceBasis::kCandidate;
        e.evidence = "commit " + sha + " and release " + key + "'s " + *r.identity.commit_sha +
                     " share only a prefix";
        prefix_candidates.push_back(std::move(e));
      }
    }
    if (exact) continue;  // exact outranks: its prefix candidates are dropped
    for (auto& e : prefix_candidates) g.candidate_edges.push_back(std::move(e));
    // A release known only by its commit.
    const std::string key = s->release.key();
    Release& r = g.releases[key];
    r.key = key;
    merge_identity(r, s->release, s->provider + " " + s->kind);
    r.signal_ids.push_back(s->id);
    widen(r, s->occurred_at);
    if (s->kind == "deploy" && !failed(*s)) {
      const std::string at = s->occurred_at;
      if (!r.deployed_at || at < *r.deployed_at) r.deployed_at = at;
    }
    CorrelationEdge e;
    e.from_id = s->id;
    e.to_id = release_node(key);
    e.relation = relation_for(*s);
    e.basis = EvidenceBasis::kExact;
    e.evidence = s->provider + " " + s->kind + " names commit " + short_sha(sha);
    g.edges.push_back(std::move(e));
  }

  // Two releases with the same version where one lacks the build or the app:
  // they may be the same build, and only a candidate says so.
  for (auto a = g.releases.begin(); a != g.releases.end(); ++a) {
    for (auto b = std::next(a); b != g.releases.end(); ++b) {
      const auto& ia = a->second.identity;
      const auto& ib = b->second.identity;
      if (!ia.version || !ib.version || *ia.version != *ib.version) continue;
      const bool builds_differ = ia.build_number && ib.build_number && *ia.build_number != *ib.build_number;
      const bool apps_differ = ia.bundle_or_package_id && ib.bundle_or_package_id &&
                               *ia.bundle_or_package_id != *ib.bundle_or_package_id;
      if (builds_differ || apps_differ) continue;
      CorrelationEdge e;
      e.from_id = release_node(a->first);
      e.to_id = release_node(b->first);
      e.relation = "same_version";
      e.basis = EvidenceBasis::kCandidate;
      e.evidence = "both name version " + *ia.version +
                   "; one of them does not say which build or app, so they may differ";
      g.candidate_edges.push_back(std::move(e));
    }
  }

  // 3. DevX sessions.
  const std::set<std::string> apps(in.workspace.app_identifiers.begin(), in.workspace.app_identifiers.end());
  for (const auto& sess : in.sessions) {
    if (!apps.empty() && !apps.count(sess.app_identifier)) continue;
    g.sessions[sess.id] = sess;
    const std::string node = "session:" + sess.id;
    if (const auto link = in.session_links.find(sess.id); link != in.session_links.end()) {
      auto it = g.releases.find(link->second);
      if (it != g.releases.end()) {
        it->second.session_ids.push_back(sess.id);
        widen(it->second, sess.created_at);
        CorrelationEdge e;
        e.from_id = release_node(link->second);
        e.to_id = node;
        e.relation = "measured_by";
        e.basis = EvidenceBasis::kProviderAttributed;
        e.evidence = "you linked this session to release " + link->second;
        e.source_ref = "user";
        g.edges.push_back(std::move(e));
        continue;
      }
      g.problems.push_back("session " + sess.id + " is linked to release " + link->second +
                           ", which no evidence names");
    }
    if (!sess.version) continue;
    for (auto& [key, r] : g.releases) {
      const auto& id = r.identity;
      if (!id.version || *id.version != *sess.version) continue;
      if (id.bundle_or_package_id && *id.bundle_or_package_id != sess.app_identifier) continue;
      const bool exact = id.bundle_or_package_id && id.build_number && sess.build &&
                         *id.build_number == *sess.build;
      if (id.build_number && sess.build && *id.build_number != *sess.build) continue;
      CorrelationEdge e;
      e.from_id = release_node(key);
      e.to_id = node;
      e.relation = "measured_by";
      if (exact) {
        e.basis = EvidenceBasis::kExact;
        e.evidence = "the session's app " + sess.app_identifier + " " + *sess.version + " (" +
                     *sess.build + ") is release " + key + " (" + sess.version_source + ")";
        r.session_ids.push_back(sess.id);
        widen(r, sess.created_at);
        g.edges.push_back(std::move(e));
      } else {
        e.basis = EvidenceBasis::kCandidate;
        e.evidence = "the session's app is version " + *sess.version +
                     " but its build, or the release's, is not known";
        g.candidate_edges.push_back(std::move(e));
      }
    }
  }

  // 4. Timing, for production evidence that names no release at all: the
  //    latest successful deploy to the same environment before it. A
  //    candidate, never more -- many things ship in a day.
  std::vector<const SignalRecord*> deploys;
  for (const auto& [id, s] : g.signals) {
    if (s.kind == "deploy" && !failed(s)) deploys.push_back(&s);
  }
  for (const auto& [id, s] : g.signals) {
    if (!is_production(s) || !s.release.empty()) continue;
    const auto t = signals::parse_iso8601(s.occurred_at);
    if (!t) continue;
    const SignalRecord* best = nullptr;
    std::int64_t best_t = 0;
    for (const SignalRecord* d : deploys) {
      if (s.environment && d->environment && *s.environment != *d->environment) continue;
      const auto dt = signals::parse_iso8601(d->occurred_at);
      if (!dt || *dt > *t || *t - *dt > 7 * 86400) continue;
      if (best == nullptr || *dt > best_t) {
        best = d;
        best_t = *dt;
      }
    }
    if (best == nullptr) continue;
    CorrelationEdge e;
    e.from_id = id;
    e.to_id = best->id;
    e.relation = "observed_in";
    e.basis = EvidenceBasis::kCandidate;
    const std::int64_t minutes = (*t - best_t) / 60;
    e.evidence = "first seen " + std::to_string(minutes) + " min after deploy " + best->external_id +
                 (best->release.commit_sha ? " of " + short_sha(*best->release.commit_sha) : std::string()) +
                 "; the signal names no release, so this is timing only";
    g.candidate_edges.push_back(std::move(e));
  }
  return g;
}

Graph load_graph(const signals::SignalStore& store, const std::string& sessions_dir,
                 const std::string& ws, std::string* error) {
  Inputs in;
  auto w = store.workspace(ws, error);
  if (!w) return Graph{};
  in.workspace = *w;
  std::vector<std::string> problems;
  in.signals = store.all_signals(ws, &problems);
  in.sessions = scan_sessions(sessions_dir);
  in.session_links = session_links(store, ws);
  Graph g = build_graph(in);
  g.problems.insert(g.problems.end(), problems.begin(), problems.end());
  return g;
}

std::map<std::string, std::string> session_links(const signals::SignalStore& store,
                                                 const std::string& ws) {
  std::map<std::string, std::string> out;
  if (!signals::id_is_safe(ws)) return out;
  if (auto v = read_json_file(store.workspace_dir(ws) + "/links/sessions.json", 4 << 20)) {
    for (const auto& [k, val] : v->members()) {
      if (val.is_string()) out[k] = val.as_string();
    }
  }
  return out;
}

bool link_session(const signals::SignalStore& store, const std::string& ws,
                  const std::string& session_id, const std::string& release_key, bool linked,
                  std::string* error) {
  std::string err;
  if (!store.workspace(ws, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  if (session_id.rfind("s-", 0) != 0 || session_id.size() > 64 ||
      session_id.find('/') != std::string::npos) {
    if (error != nullptr) *error = "not a session id: '" + session_id + "'";
    return false;
  }
  auto links = session_links(store, ws);
  if (linked) {
    if (release_key.empty() || release_key.size() > 200) {
      if (error != nullptr) *error = "no release to link to";
      return false;
    }
    links[session_id] = release_key;
  } else {
    links.erase(session_id);
  }
  json::Value o = json::Value::object();
  for (const auto& [k, v] : links) o.set(k, json::Value::string(v));
  if (write_private(store.workspace_dir(ws) + "/links/sessions.json", o.dump(1) + "\n") != 0) {
    if (error != nullptr) *error = "could not write the session links";
    return false;
  }
  return true;
}

json::Value releases_json(const Graph& g) {
  std::vector<const Release*> rs;
  for (const auto& [_, r] : g.releases) rs.push_back(&r);
  std::sort(rs.begin(), rs.end(), [](const Release* a, const Release* b) {
    return a->last_activity_at > b->last_activity_at;
  });
  json::Value a = json::Value::array();
  for (const Release* r : rs) {
    json::Value o = json::Value::object();
    o.set("key", json::Value::string(r->key));
    o.set("identity", r->identity.to_json());
    std::map<std::string, int> kinds;
    std::set<std::string> providers;
    int crashes = 0, issues = 0, ci_failures = 0;
    for (const auto& id : r->signal_ids) {
      const auto it = g.signals.find(id);
      if (it == g.signals.end()) continue;
      const auto& s = it->second;
      kinds[s.kind]++;
      providers.insert(s.provider);
      if (s.kind == "crash") crashes++;
      if (s.kind == "issue") issues++;
      if (is_ci(s) && failed(s)) ci_failures++;
    }
    json::Value k = json::Value::object();
    for (const auto& [kind, n] : kinds) k.set(kind, json::Value::integer(n));
    o.set("signals_by_kind", std::move(k));
    json::Value p = json::Value::array();
    for (const auto& x : providers) p.push_back(json::Value::string(x));
    o.set("providers", std::move(p));
    o.set("crashes", json::Value::integer(crashes));
    o.set("issues", json::Value::integer(issues));
    o.set("ci_failures", json::Value::integer(ci_failures));
    o.set("sessions", json::Value::integer(static_cast<std::int64_t>(r->session_ids.size())));
    o.set("first_activity_at", json::Value::string(r->first_activity_at));
    o.set("last_activity_at", json::Value::string(r->last_activity_at));
    if (r->deployed_at) o.set("deployed_at", json::Value::string(*r->deployed_at));
    o.set("conflicts", json::Value::integer(static_cast<std::int64_t>(r->conflicts.size())));
    a.push_back(std::move(o));
  }
  json::Value out = json::Value::object();
  out.set("workspace", json::Value::string(g.workspace_id));
  out.set("releases", std::move(a));
  json::Value pr = json::Value::array();
  for (const auto& x : g.problems) pr.push_back(json::Value::string(x));
  out.set("problems", std::move(pr));
  return out;
}

json::Value release_overview(const Graph& g, const std::string& key, const json::Value& freshness) {
  json::Value o = json::Value::object();
  const auto it = g.releases.find(key);
  if (it == g.releases.end()) {
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string("no release '" + key + "' in workspace '" + g.workspace_id +
                                       "' (mpi intelligence releases lists them)"));
    return o;
  }
  const Release& r = it->second;
  o.set("ok", json::Value::boolean(true));
  o.set("key", json::Value::string(r.key));
  o.set("identity", r.identity.to_json());
  json::Value conflicts = json::Value::array();
  for (const auto& c : r.conflicts) conflicts.push_back(json::Value::string(c));
  o.set("conflicts", std::move(conflicts));
  if (r.deployed_at) o.set("deployed_at", json::Value::string(*r.deployed_at));

  // Evidence by provider, and the timeline.
  std::map<std::string, std::vector<const SignalRecord*>> by_provider;
  struct Step { std::string at, what, ref; };
  std::vector<Step> steps;
  std::optional<std::string> first_prod;
  std::set<std::string> environments;
  bool has_ci = false, has_prod = false, has_deploy = false;
  for (const auto& id : r.signal_ids) {
    const auto sit = g.signals.find(id);
    if (sit == g.signals.end()) continue;
    const auto& s = sit->second;
    by_provider[s.provider].push_back(&s);
    if (s.environment) environments.insert(*s.environment);
    if (s.kind == "pipeline") {
      has_ci = true;
      steps.push_back({s.occurred_at, "pipeline " + jstr(s.attributes, "status"), s.id});
    } else if (s.kind == "ci_job") {
      has_ci = true;
      if (failed(s)) steps.push_back({s.occurred_at, "job failed: " + s.title.value_or(s.external_id), s.id});
    } else if (s.kind == "test") {
      has_ci = true;
      steps.push_back({s.occurred_at, "tests: " + s.title.value_or("summary"), s.id});
    } else if (s.kind == "deploy") {
      has_deploy = true;
      steps.push_back({s.occurred_at, "deploy to " + s.environment.value_or("?") + ": " +
                                          jstr(s.attributes, "status"), s.id});
    } else if (is_production(s)) {
      has_prod = true;
      if (!first_prod || s.occurred_at < *first_prod) first_prod = s.occurred_at;
    }
  }
  if (first_prod) steps.push_back({*first_prod, "first production signal", ""});
  for (const auto& sid : r.session_ids) {
    const auto sit = g.sessions.find(sid);
    if (sit != g.sessions.end()) {
      steps.push_back({sit->second.created_at,
                       std::string(sit->second.browserstack ? "BrowserStack" : "DevX") + " session", sid});
    }
  }
  std::sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
  json::Value tl = json::Value::array();
  for (const auto& st : steps) {
    json::Value one = json::Value::object();
    one.set("at", json::Value::string(st.at));
    one.set("what", json::Value::string(st.what));
    if (!st.ref.empty()) one.set("ref", json::Value::string(st.ref));
    tl.push_back(std::move(one));
  }
  o.set("timeline", std::move(tl));
  json::Value sections = json::Value::object();
  for (auto& [provider, list] : by_provider) {
    std::sort(list.begin(), list.end(), [](const SignalRecord* a, const SignalRecord* b) {
      return a->occurred_at > b->occurred_at;
    });
    json::Value a = json::Value::array();
    for (const SignalRecord* s : list) a.push_back(entry_json(*s));
    sections.set(provider, std::move(a));
  }
  o.set("evidence", std::move(sections));
  json::Value envs = json::Value::array();
  for (const auto& e : environments) envs.push_back(json::Value::string(e));
  o.set("environments", std::move(envs));
  json::Value sess = json::Value::array();
  for (const auto& sid : r.session_ids) {
    const auto sit = g.sessions.find(sid);
    if (sit != g.sessions.end()) sess.push_back(sit->second.to_json());
  }
  o.set("sessions", std::move(sess));

  // Links touching the release or its members, exact and candidate apart.
  std::set<std::string> nodes(r.signal_ids.begin(), r.signal_ids.end());
  nodes.insert(release_node(key));
  for (const auto& sid : r.session_ids) nodes.insert("session:" + sid);
  json::Value exact = json::Value::array(), candidate = json::Value::array();
  for (const auto& e : g.edges) {
    if (nodes.count(e.from_id) || nodes.count(e.to_id)) exact.push_back(e.to_json());
  }
  for (const auto& e : g.candidate_edges) {
    if (nodes.count(e.from_id) || nodes.count(e.to_id)) candidate.push_back(e.to_json());
  }
  o.set("exact_links", std::move(exact));
  o.set("candidate_links", std::move(candidate));

  json::Value missing = json::Value::array();
  auto miss = [&](const std::string& m) { missing.push_back(json::Value::string(m)); };
  if (!r.identity.commit_sha) miss("no commit SHA is known for this release, so CI and code cannot be joined exactly");
  if (!r.identity.build_number && r.identity.version) miss("no build number: sessions can only be candidates");
  if (!has_ci) miss("no CI evidence (pipeline, job, tests) is linked");
  if (!has_deploy) miss("no deployment is recorded");
  if (!has_prod) miss("no production signals (crashes, issues, metrics) are linked");
  if (r.session_ids.empty()) miss("no DevX or BrowserStack session measured this build");
  o.set("missing_evidence", std::move(missing));
  o.set("freshness", freshness);
  return o;
}

json::Value release_compare(const Graph& g, const std::string& base, const std::string& cand) {
  json::Value o = json::Value::object();
  for (const auto* k : {&base, &cand}) {
    if (!g.releases.count(*k)) {
      o.set("ok", json::Value::boolean(false));
      o.set("error", json::Value::string("no release '" + *k + "'"));
      return o;
    }
  }
  struct Sum {
    std::map<std::string, int> kinds;
    int crashes = 0, issues = 0, ci_failures = 0, tests_failed = 0;
    bool has_tests = false;
    int events_from = 0, users_from = 0;  // how many issues reported each
    std::optional<double> events, users;
    std::set<std::string> titles;
    std::map<std::string, const SignalRecord*> metrics;
  };
  auto summarise = [&](const Release& r) {
    Sum s;
    for (const auto& id : r.signal_ids) {
      const auto it = g.signals.find(id);
      if (it == g.signals.end()) continue;
      const auto& x = it->second;
      s.kinds[x.kind]++;
      if (x.kind == "crash" || x.kind == "issue") {
        (x.kind == "crash" ? s.crashes : s.issues)++;
        if (x.title) s.titles.insert(*x.title);
        // Event and user totals add only what was reported; absent stays absent.
        for (const char* k : {"count", "events"}) {
          if (auto v = jnum(x.attributes, k)) {
            s.events = s.events.value_or(0) + *v;
            s.events_from++;
            break;
          }
        }
        for (const char* k : {"user_count", "affected_installations"}) {
          if (auto v = jnum(x.attributes, k)) {
            s.users = s.users.value_or(0) + *v;
            s.users_from++;
            break;
          }
        }
      }
      if (is_ci(x) && failed(x)) s.ci_failures++;
      if (x.kind == "test") {
        s.has_tests = true;
        if (auto v = jnum(x.attributes, "failed")) s.tests_failed += static_cast<int>(*v);
      }
      if (x.kind == "metric") s.metrics[metric_key(x)] = &x;
    }
    return s;
  };
  const Sum a = summarise(g.releases.at(base));
  const Sum b = summarise(g.releases.at(cand));
  auto side = [](const Sum& s) {
    json::Value v = json::Value::object();
    json::Value k = json::Value::object();
    for (const auto& [kind, n] : s.kinds) k.set(kind, json::Value::integer(n));
    v.set("signals_by_kind", std::move(k));
    v.set("crashes", json::Value::integer(s.crashes));
    v.set("issues", json::Value::integer(s.issues));
    v.set("ci_failures", json::Value::integer(s.ci_failures));
    // No test summary: absent, not zero failures.
    if (s.has_tests) v.set("tests_failed", json::Value::integer(s.tests_failed));
    // Sums over the issues that reported a number, and how many those were:
    // a total of three issues out of nine is not the release's total.
    if (s.events) {
      v.set("events", json::Value::number(*s.events));
      v.set("events_reported_by", json::Value::string(std::to_string(s.events_from) + " of " +
                                                      std::to_string(s.crashes + s.issues) + " issues"));
    }
    if (s.users) {
      v.set("affected_users", json::Value::number(*s.users));
      v.set("affected_users_reported_by", json::Value::string(std::to_string(s.users_from) + " of " +
                                                              std::to_string(s.crashes + s.issues) + " issues"));
    }
    return v;
  };
  o.set("ok", json::Value::boolean(true));
  o.set("base", json::Value::string(base));
  o.set("candidate", json::Value::string(cand));
  o.set("base_summary", side(a));
  o.set("candidate_summary", side(b));
  json::Value fresh = json::Value::array();
  for (const auto& t : b.titles) {
    if (!a.titles.count(t)) fresh.push_back(json::Value::string(t));
  }
  o.set("issues_only_in_candidate", std::move(fresh));
  json::Value metrics = json::Value::array();
  for (const auto& [mk, mb] : b.metrics) {
    const auto ma = a.metrics.find(mk);
    if (ma == a.metrics.end()) continue;
    json::Value m = json::Value::object();
    m.set("metric", json::Value::string(mk));
    m.set("unit", json::Value::string(jstr(mb->attributes, "unit")));
    for (const char* p : {"p50", "p90", "p95", "p99"}) {
      const auto va = jnum(ma->second->attributes, p);
      const auto vb = jnum(mb->attributes, p);
      if (!va || !vb) continue;
      json::Value d = json::Value::object();
      d.set("base", json::Value::number(*va));
      d.set("candidate", json::Value::number(*vb));
      d.set("delta", json::Value::number(*vb - *va));
      if (*va != 0) d.set("relative", json::Value::number((*vb - *va) / *va));
      m.set(p, std::move(d));
    }
    metrics.push_back(std::move(m));
  }
  o.set("metric_deltas", std::move(metrics));
  o.set("note", json::Value::string(
                    "Counts are of signals each provider reported for each release. Event and user "
                    "totals add only the issues that reported one (see *_reported_by), and a Sentry "
                    "issue's count is its lifetime total across releases, not this release's alone. "
                    "A difference is a difference in evidence, not a measured cause."));
  return o;
}

json::Value related(const Graph& g, const std::string& signal_id) {
  json::Value o = json::Value::object();
  const auto it = g.signals.find(signal_id);
  if (it == g.signals.end()) {
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string("no signal '" + signal_id + "'"));
    return o;
  }
  o.set("ok", json::Value::boolean(true));
  o.set("signal", entry_json(it->second));
  json::Value exact = json::Value::array(), candidate = json::Value::array();
  std::set<std::string> releases;
  for (const auto& e : g.edges) {
    if (e.from_id == signal_id || e.to_id == signal_id) {
      exact.push_back(e.to_json());
      for (const auto* n : {&e.from_id, &e.to_id}) {
        if (n->rfind("release:", 0) == 0) releases.insert(n->substr(8));
      }
    }
  }
  for (const auto& e : g.candidate_edges) {
    if (e.from_id == signal_id || e.to_id == signal_id) candidate.push_back(e.to_json());
  }
  o.set("exact_links", std::move(exact));
  o.set("candidate_links", std::move(candidate));
  json::Value rel = json::Value::array();
  for (const auto& key : releases) {
    const Release& r = g.releases.at(key);
    json::Value one = json::Value::object();
    one.set("key", json::Value::string(key));
    one.set("identity", r.identity.to_json());
    json::Value others = json::Value::array();
    for (const auto& sid : r.signal_ids) {
      if (sid == signal_id || others.size() >= 50) continue;
      const auto sit = g.signals.find(sid);
      if (sit != g.signals.end()) others.push_back(entry_json(sit->second));
    }
    one.set("other_evidence", std::move(others));
    json::Value sess = json::Value::array();
    for (const auto& sid : r.session_ids) sess.push_back(json::Value::string(sid));
    one.set("sessions", std::move(sess));
    rel.push_back(std::move(one));
  }
  o.set("releases", std::move(rel));
  return o;
}

json::Value overview(const Graph& g, const json::Value& integrations) {
  json::Value o = json::Value::object();
  o.set("workspace", json::Value::string(g.workspace_id));
  const json::Value rels = releases_json(g);
  json::Value latest = json::Value::array();
  if (const json::Value* a = rels.find("releases")) {
    for (const auto& r : a->items()) {
      if (latest.size() >= 5) break;
      latest.push_back(r);
    }
  }
  o.set("latest_releases", std::move(latest));
  std::map<std::string, int> by_provider, by_kind;
  int prod = 0, ci = 0, ci_failed = 0, tests = 0;
  for (const auto& [_, s] : g.signals) {
    by_provider[s.provider]++;
    by_kind[s.kind]++;
    if (is_production(s)) prod++;
    if (is_ci(s) || s.kind == "deploy") ci++;
    if (is_ci(s) && failed(s)) ci_failed++;
    if (s.kind == "test") tests++;
  }
  json::Value cards = json::Value::object();
  auto card = [&](const char* name, int n, const std::string& note) {
    json::Value c = json::Value::object();
    c.set("count", json::Value::integer(n));
    c.set("note", json::Value::string(note));
    cards.set(name, std::move(c));
  };
  card("production", prod, std::to_string(by_kind["crash"]) + " crash, " + std::to_string(by_kind["issue"]) +
                               " issue, " + std::to_string(by_kind["metric"]) + " metric signals");
  card("ci_cd", ci, std::to_string(ci_failed) + " failed pipeline/job/test signals");
  card("tests", tests, "test summaries");
  card("local_sessions", static_cast<int>(g.sessions.size()), "DevX and BrowserStack sessions of this workspace's apps");
  o.set("cards", std::move(cards));
  json::Value bp = json::Value::object();
  for (const auto& [k, v] : by_provider) bp.set(k, json::Value::integer(v));
  o.set("signals_by_provider", std::move(bp));
  // Freshness and attention, straight from the connectors' own state.
  json::Value attention = json::Value::array();
  json::Value freshness = json::Value::object();
  if (const json::Value* cs = integrations.find("connectors"); cs != nullptr && cs->is_array()) {
    for (const auto& c : cs->items()) {
      const json::Value* cfg = c.find("config");
      const std::string id = cfg != nullptr ? jstr(*cfg, "id") : "";
      const json::Value* h = c.find("health");
      const std::string state = h != nullptr ? jstr(*h, "state") : "";
      const json::Value* sync = c.find("sync");
      json::Value f = json::Value::object();
      f.set("state", json::Value::string(state));
      if (sync != nullptr) {
        if (const json::Value* ls = sync->find("last_success_at")) f.set("last_success_at", *ls);
        if (const json::Value* la = sync->find("last_attempt_at")) f.set("last_attempt_at", *la);
      }
      freshness.set(id, std::move(f));
      if (state != "up_to_date" && state != "paused") {
        attention.push_back(json::Value::string(id + ": " + state +
                                                (h != nullptr && !jstr(*h, "detail").empty()
                                                     ? " — " + jstr(*h, "detail") : "")));
      }
    }
  }
  if (ci_failed > 0) attention.push_back(json::Value::string(std::to_string(ci_failed) + " failed CI signal(s)"));
  o.set("freshness", std::move(freshness));
  o.set("needs_attention", std::move(attention));
  o.set("candidate_links", json::Value::integer(static_cast<std::int64_t>(g.candidate_edges.size())));
  o.set("exact_links", json::Value::integer(static_cast<std::int64_t>(g.edges.size())));
  return o;
}

}  // namespace mpi::correlation
