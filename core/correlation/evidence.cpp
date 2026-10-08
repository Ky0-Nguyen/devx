#include "core/correlation/evidence.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "core/code/repository.hpp"
#include "core/signals/redact.hpp"

namespace mpi::correlation {
namespace {

using signals::SignalRecord;

std::string jstr(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

std::optional<std::int64_t> jint(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  if (v == nullptr || !v->is_number()) return std::nullopt;
  return v->as_int();
}

json::Value redaction_json(const signals::Redaction& r) {
  json::Value o = json::Value::object();
  for (const auto& [k, n] : r.removed) o.set(k, json::Value::integer(n));
  return o;
}

// The excerpt of a signal's raw evidence a person or a model should see.
json::Value raw_excerpt(const signals::SignalStore& store, const std::string& ws, const SignalRecord& s,
                        std::size_t max_bytes) {
  json::Value o = json::Value::object();
  o.set("signal_id", json::Value::string(s.id));
  o.set("raw_ref", json::Value::string(s.raw_ref));
  if (s.raw_ref.empty()) {
    o.set("available", json::Value::boolean(false));
    o.set("why", json::Value::string("the connector kept no raw evidence for this signal"));
    return o;
  }
  const auto start = jint(s.attributes, "excerpt_start");
  const auto end = jint(s.attributes, "excerpt_end");
  std::string text;
  std::string what;
  if (start && end && *start > 0 && *end >= *start) {
    // A CI log: stream to the located window rather than load the log.
    const auto r = store.read_raw_lines(ws, s.raw_ref, static_cast<std::size_t>(*start),
                                        static_cast<std::size_t>(*end), max_bytes);
    if (!r.ok) {
      o.set("available", json::Value::boolean(false));
      o.set("why", json::Value::string(r.error));
      return o;
    }
    text = r.bytes;
    what = "lines " + std::to_string(*start) + "-" + std::to_string(*end) + " of the log, around the root error";
  } else {
    const auto r = store.read_raw(ws, s.raw_ref, 0, max_bytes);
    if (!r.ok) {
      o.set("available", json::Value::boolean(false));
      o.set("why", json::Value::string(r.error));
      return o;
    }
    text = r.bytes;
    what = r.truncated ? "the first " + std::to_string(r.bytes.size()) + " of " + std::to_string(r.size) + " bytes"
                       : "the whole raw evidence";
  }
  const signals::Redaction red = signals::redact(text);
  o.set("available", json::Value::boolean(true));
  o.set("what", json::Value::string(what));
  o.set("text", json::Value::string(red.text));
  o.set("redacted", redaction_json(red));
  return o;
}

// Stack frames from whatever shape the connector stored.
std::vector<json::Value> frames_of(const SignalRecord& s) {
  std::vector<json::Value> out;
  if (const json::Value* f = s.attributes.find("frames"); f != nullptr && f->is_array()) {
    for (const auto& x : f->items()) {
      if (x.is_object()) out.push_back(x);
    }
  }
  if (out.empty()) {
    if (const json::Value* b = s.attributes.find("blame_frame"); b != nullptr && b->is_object()) out.push_back(*b);
  }
  return out;
}

std::string frame_file(const json::Value& f) {
  for (const char* k : {"file", "filename", "abs_path", "path"}) {
    const std::string v = jstr(f, k);
    if (!v.empty()) return v;
  }
  return {};
}

int frame_line(const json::Value& f) {
  for (const char* k : {"line", "lineno", "lineNo"}) {
    if (auto v = jint(f, k)) return static_cast<int>(*v);
  }
  return 0;
}

const Release* release_of(const Graph& g, const std::string& signal_id) {
  for (const auto& e : g.edges) {
    if (e.from_id == signal_id && e.to_id.rfind("release:", 0) == 0) {
      const auto it = g.releases.find(e.to_id.substr(8));
      if (it != g.releases.end()) return &it->second;
    }
  }
  return nullptr;
}

// The previous release of the same app with a known commit, by activity.
const Release* previous_release(const Graph& g, const Release& r) {
  const Release* best = nullptr;
  for (const auto& [_, other] : g.releases) {
    if (&other == &r || !other.identity.commit_sha) continue;
    if (r.identity.bundle_or_package_id && other.identity.bundle_or_package_id &&
        *r.identity.bundle_or_package_id != *other.identity.bundle_or_package_id) {
      continue;
    }
    if (other.last_activity_at >= r.first_activity_at) continue;
    if (best == nullptr || other.last_activity_at > best->last_activity_at) best = &other;
  }
  return best;
}

}  // namespace

json::Value signal_read(const signals::SignalStore& store, const std::string& ws,
                        const std::string& signal_id, bool include_raw) {
  json::Value o = json::Value::object();
  std::string err;
  auto s = store.signal(ws, signal_id, &err);
  if (!s) {
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string(err));
    return o;
  }
  o.set("ok", json::Value::boolean(true));
  // Normalized fields can carry provider text too (a CI log's root error
  // line, an issue title): redacted like everything else that leaves.
  std::map<std::string, int> counts;
  o.set("signal", signals::redact_json(s->to_json(), /*pii=*/true, /*ip_addresses=*/false, &counts));
  if (!counts.empty()) {
    json::Value c = json::Value::object();
    for (const auto& [k, n] : counts) c.set(k, json::Value::integer(n));
    o.set("signal_redacted", std::move(c));
  }
  if (include_raw) {
    o.set("raw", raw_excerpt(store, ws, *s, kMaxExcerptBytes));
    o.set("raw_warning", json::Value::string(
                             "Raw provider evidence can contain personal data and secrets. This "
                             "excerpt was redacted for known credential and contact shapes; a "
                             "shape the scanner does not know passes through."));
  }
  return o;
}

json::Value code_context_for_signal(const signals::SignalStore& store, const Graph& g,
                                    const std::string& ws, const std::string& signal_id) {
  json::Value o = json::Value::object();
  std::string err;
  auto w = store.workspace(ws, &err);
  auto s = store.signal(ws, signal_id, &err);
  if (!w || !s) {
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string(err));
    return o;
  }
  code::GitRepository repo(w->repository_root);
  const code::RepositoryInfo info = repo.repository();
  o.set("repository", info.to_json());
  if (!info.ok) {
    // Unavailable, not broken: everything else still works.
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string(info.error));
    return o;
  }
  o.set("ok", json::Value::boolean(true));
  std::optional<std::string> commit = s->release.commit_sha;
  const Release* rel = release_of(g, s->id);
  if (!commit && rel != nullptr && rel->identity.commit_sha) commit = rel->identity.commit_sha;
  std::string ref = "HEAD";
  json::Value notes = json::Value::array();
  if (commit && repo.has_commit(*commit)) {
    ref = *commit;
    o.set("commit", repo.commit(*commit).to_json());
    o.set("ref_basis", json::Value::string("exact: the release's commit"));
  } else {
    o.set("ref_basis", json::Value::string(
                           commit ? "HEAD: the release's commit " + *commit + " is not in this repository"
                                  : "HEAD: no commit is known for this signal's release"));
  }

  // Frames, crash frame first; in-app ones first when marked.
  std::vector<json::Value> frames = frames_of(*s);
  std::stable_sort(frames.begin(), frames.end(), [](const json::Value& a, const json::Value& b) {
    const json::Value* ia = a.find("in_app");
    const json::Value* ib = b.find("in_app");
    return (ia != nullptr && ia->as_bool(false)) && !(ib != nullptr && ib->as_bool(false));
  });
  json::Value resolved = json::Value::array();
  std::vector<std::string> files;
  for (const auto& f : frames) {
    if (resolved.size() >= 5) break;
    const std::string file = frame_file(f);
    if (file.empty()) continue;
    json::Value one = json::Value::object();
    one.set("frame", f);
    const auto path = repo.resolve_path(file);
    if (!path) {
      one.set("resolved", json::Value::boolean(false));
      one.set("why", json::Value::string("no single tracked file matches '" + file + "'"));
      resolved.push_back(std::move(one));
      continue;
    }
    one.set("resolved", json::Value::boolean(true));
    one.set("path", json::Value::string(*path));
    const int line = frame_line(f);
    if (line > 0) {
      one.set("source", repo.lines(*path, ref, line - 5, line + 5).to_json());
      one.set("blame", repo.blame(*path, line, ref).to_json());
    }
    if (std::find(files.begin(), files.end(), *path) == files.end()) files.push_back(*path);
    resolved.push_back(std::move(one));
  }
  o.set("frames", std::move(resolved));

  // What the release changed in those files, against the previous release
  // (or the commit's parent).
  if (ref != "HEAD") {
    std::string base;
    if (rel != nullptr) {
      if (const Release* prev = previous_release(g, *rel); prev != nullptr && repo.has_commit(*prev->identity.commit_sha)) {
        base = *prev->identity.commit_sha;
        o.set("diff_base", json::Value::string("previous release " + prev->key));
      }
    }
    if (base.empty()) o.set("diff_base", json::Value::string("the commit's parent"));
    o.set("diff", repo.diff(base, ref, 16 * 1024, files).to_json());
    if (!files.empty()) {
      // And every file it touched, without patches, for scope.
      auto all = repo.diff(base, ref, 0, {});
      all.patch.clear();
      o.set("changed_files", all.to_json());
    }
  }
  if (frames.empty()) {
    notes.push_back(json::Value::string("this signal carries no stack frames"));
  }
  o.set("notes", std::move(notes));
  return o;
}

json::Value evidence_pack(const signals::SignalStore& store, const Graph& g, const std::string& ws,
                          const PackScope& scope, const json::Value& freshness) {
  json::Value pack = json::Value::object();
  json::Value qs = json::Value::object();
  qs.set("workspace", json::Value::string(ws));
  std::string key = scope.release;
  if (!scope.signal_id.empty()) {
    qs.set("signal", json::Value::string(scope.signal_id));
    if (key.empty()) {
      if (const Release* r = release_of(g, scope.signal_id)) key = r->key;
    }
  }
  if (!key.empty()) qs.set("release", json::Value::string(key));
  if (!scope.question.empty()) qs.set("question", json::Value::string(scope.question));
  pack.set("question_scope", std::move(qs));
  pack.set("freshness", freshness);
  pack.set("instructions", json::Value::string(
                               "Reason over these facts and cite their source_refs. Exact links are "
                               "deterministic identity; candidate links are timing or partial metadata "
                               "and are not causes. Numbers here were computed by DevX: do not "
                               "recompute percentiles or infer joins. Ask for more with signal_read, "
                               "signal_related or code_context_for_signal."));

  json::Value facts = json::Value::array();
  json::Value refs = json::Value::array();
  std::set<std::string> ref_set;
  auto cite = [&](const std::string& id) {
    if (!id.empty() && ref_set.insert(id).second) refs.push_back(json::Value::string(id));
  };
  json::Value exact = json::Value::array(), candidate = json::Value::array(), missing = json::Value::array();
  std::vector<const SignalRecord*> members;
  if (!key.empty() && g.releases.count(key)) {
    const Release& r = g.releases.at(key);
    const json::Value ov = release_overview(g, key, freshness);
    std::string head = "Release " + key;
    if (r.identity.commit_sha) head += ", commit " + *r.identity.commit_sha;
    if (r.deployed_at) head += ", first deployed " + *r.deployed_at;
    facts.push_back(json::Value::string(head));
    for (const auto& c : r.conflicts) facts.push_back(json::Value::string("conflict: " + c));
    for (const auto& id : r.signal_ids) {
      const auto it = g.signals.find(id);
      if (it != g.signals.end()) members.push_back(&it->second);
    }
    for (const auto& sid : r.session_ids) {
      const auto it = g.sessions.find(sid);
      if (it == g.sessions.end()) continue;
      facts.push_back(json::Value::string(std::string(it->second.browserstack ? "BrowserStack" : "DevX") +
                                          " session " + sid + " measured this build on " +
                                          it->second.device_id + " (" + it->second.created_at + ")"));
      cite(sid);
    }
    if (const json::Value* a = ov.find("exact_links")) exact = *a;
    if (const json::Value* a = ov.find("candidate_links")) candidate = *a;
    if (const json::Value* a = ov.find("missing_evidence")) missing = *a;
  } else if (!key.empty()) {
    missing.push_back(json::Value::string("no release '" + key + "' is known"));
  }
  // Whether members[0] is the focused signal, which then stays first.
  bool focused_first = false;
  if (!scope.signal_id.empty()) {
    const auto it = g.signals.find(scope.signal_id);
    if (it == g.signals.end()) {
      missing.push_back(json::Value::string("no signal '" + scope.signal_id + "'"));
    } else if (auto at = std::find(members.begin(), members.end(), &it->second); at != members.end()) {
      std::rotate(members.begin(), at, at + 1);
      focused_first = true;
    } else {
      members.insert(members.begin(), &it->second);
      focused_first = true;
      const json::Value rel = related(g, scope.signal_id);
      if (const json::Value* a = rel.find("exact_links")) {
        for (const auto& e : a->items()) exact.push_back(e);
      }
      if (const json::Value* a = rel.find("candidate_links")) {
        for (const auto& e : a->items()) candidate.push_back(e);
      }
    }
  }
  // The focused signal first, then failures and crashes, then the rest.
  std::stable_sort(members.begin() + (focused_first ? 1 : 0), members.end(),
                   [](const SignalRecord* a, const SignalRecord* b) {
                     auto rank = [](const SignalRecord* s) {
                       if (s->kind == "crash") return 0;
                       if (s->severity && (*s->severity == "error" || *s->severity == "fatal")) return 1;
                       if (s->kind == "issue") return 2;
                       if (s->kind == "metric") return 3;
                       return 4;
                     };
                     return rank(a) < rank(b);
                   });
  for (const SignalRecord* s : members) {
    if (facts.size() >= 60) break;
    std::string f = s->provider + " " + s->kind + " " + s->external_id;
    if (s->title) f += ": " + s->title->substr(0, 160);
    if (s->severity) f += " [" + *s->severity + "]";
    if (s->environment) f += " in " + *s->environment;
    f += " at " + s->occurred_at;
    for (const char* k : {"count", "events", "user_count", "affected_installations", "root_error", "p50", "p95"}) {
      if (const json::Value* v = s->attributes.find(k)) {
        f += "; " + std::string(k) + "=" + (v->is_string() ? v->as_string().substr(0, 200) : v->dump());
      }
    }
    facts.push_back(json::Value::string(f));
    cite(s->id);
  }
  pack.set("facts", std::move(facts));
  pack.set("exact_links", std::move(exact));
  pack.set("candidate_links", std::move(candidate));

  json::Value excerpts = json::Value::array();
  if (scope.include_raw) {
    std::size_t budget = 3 * kMaxExcerptBytes;
    for (const SignalRecord* s : members) {
      if (budget < 512 || excerpts.size() >= 4) break;
      const bool worth = s->kind == "ci_job" || s->kind == "crash" || s->id == scope.signal_id;
      if (!worth || s->raw_ref.empty()) continue;
      json::Value ex = raw_excerpt(store, ws, *s, std::min(budget, kMaxExcerptBytes));
      budget -= std::min(budget, jstr(ex, "text").size());
      excerpts.push_back(std::move(ex));
    }
  }
  pack.set("raw_excerpts", std::move(excerpts));

  json::Value code = json::Value::array();
  if (scope.include_code) {
    for (const SignalRecord* s : members) {
      if (code.size() >= 2) break;
      const bool has_frames = s->attributes.find("frames") != nullptr || s->attributes.find("blame_frame") != nullptr;
      if (!has_frames && s->id != scope.signal_id) continue;
      json::Value ctx = code_context_for_signal(store, g, ws, s->id);
      // Keep the pack small: patches are trimmed again here.
      if (const json::Value* d = ctx.find("diff"); d != nullptr && d->is_object()) {
        json::Value dd = *d;
        const std::string patch = jstr(dd, "patch");
        if (patch.size() > 6 * 1024) {
          dd.set("patch", json::Value::string(patch.substr(0, 6 * 1024)));
          dd.set("patch_truncated", json::Value::boolean(true));
        }
        ctx.set("diff", std::move(dd));
      }
      ctx.set("signal_id", json::Value::string(s->id));
      code.push_back(std::move(ctx));
    }
  }
  pack.set("code_context", std::move(code));
  pack.set("missing_evidence", std::move(missing));
  pack.set("source_refs", std::move(refs));

  // Redact every string of the pack once more, then bound it. IP addresses
  // are left alone here: a four-part version number looks like one.
  std::map<std::string, int> counts;
  json::Value out = signals::redact_json(pack, /*pii=*/true, /*ip_addresses=*/false, &counts);
  if (const json::Value* ex = pack.find("raw_excerpts"); ex != nullptr) {
    // The excerpts were redacted (IPs included) when they were cut; count
    // what that removed too, so the total says what this pack lost.
    for (const auto& x : ex->items()) {
      if (const json::Value* rr = x.find("redacted"); rr != nullptr) {
        for (const auto& [k, n] : rr->members()) counts[k] += static_cast<int>(n.as_int());
      }
    }
  }
  json::Value total = json::Value::object();
  for (const auto& [k, n] : counts) total.set(k, json::Value::integer(n));
  out.set("redacted", std::move(total));
  if (out.dump().size() > kMaxPackBytes) {
    out.set("code_context", json::Value::array());
    out.set("truncated", json::Value::string("code context dropped to stay within " +
                                             std::to_string(kMaxPackBytes / 1024) +
                                             " KB; ask code_context_for_signal for it"));
    if (out.dump().size() > kMaxPackBytes) out.set("raw_excerpts", json::Value::array());
  }
  return out;
}

}  // namespace mpi::correlation
