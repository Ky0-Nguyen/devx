#include "adapters/intelligence/service.hpp"

#include <ctime>

#include "adapters/intelligence/builtin.hpp"
#include "core/correlation/engine.hpp"
#include "core/correlation/evidence.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"

namespace mpi::intelligence {
namespace {

using signals::SignalStore;

json::Value fail(const std::string& why) {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(false));
  o.set("error", json::Value::string(why));
  return o;
}

json::Value ok(json::Value o) {
  if (!o.is_object()) {
    json::Value w = json::Value::object();
    w.set("value", std::move(o));
    o = std::move(w);
  }
  if (o.find("ok") == nullptr) o.set("ok", json::Value::boolean(true));
  return o;
}

SignalStore store_for(const std::string& sessions_dir) {
  register_builtin_connectors();
  return SignalStore(sessions_dir);
}

bool has_workspace(const SignalStore& store, const std::string& ws, json::Value* error) {
  std::string err;
  if (store.workspace(ws, &err)) return true;
  *error = fail(err);
  return false;
}

std::string jstr(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

}  // namespace

json::Value providers() {
  register_builtin_connectors();
  json::Value o = json::Value::object();
  o.set("providers", signals::providers());
  return ok(std::move(o));
}

json::Value egress(const std::string& sessions_dir) {
  return ok(signals::egress_ledger(store_for(sessions_dir)));
}

json::Value workspaces(const std::string& sessions_dir) {
  SignalStore store = store_for(sessions_dir);
  std::vector<std::string> problems;
  json::Value a = json::Value::array();
  for (const auto& w : store.workspaces(&problems)) {
    json::Value one = w.to_json();
    one.set("freshness", signals::freshness(store, w.id));
    one.set("connectors", json::Value::integer(static_cast<std::int64_t>(store.connectors(w.id).size())));
    a.push_back(std::move(one));
  }
  json::Value o = json::Value::object();
  o.set("workspaces", std::move(a));
  json::Value p = json::Value::array();
  for (const auto& s : problems) p.push_back(json::Value::string(s));
  o.set("problems", std::move(p));
  return ok(std::move(o));
}

json::Value workspace_save(const std::string& sessions_dir, const json::Value& workspace) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto w = signals::ProjectWorkspace::from_json(workspace, &err);
  if (!w) return fail(err);
  if (!store.save_workspace(*w, &err)) return fail(err);
  json::Value o = json::Value::object();
  o.set("workspace", w->to_json());
  return ok(std::move(o));
}

json::Value workspace_delete(const std::string& sessions_dir, const std::string& ws) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  if (!store.workspace(ws, &err)) return fail(err);
  if (!store.delete_workspace(ws, &err)) return fail(err);
  return ok(json::Value::object());
}

json::Value integrations(const std::string& sessions_dir, const std::string& ws) {
  SignalStore store = store_for(sessions_dir);
  json::Value e;
  if (!has_workspace(store, ws, &e)) return e;
  json::Value o = signals::integrations(store, ws);
  o.set("providers", signals::providers());
  return ok(std::move(o));
}

json::Value connector_save(const std::string& sessions_dir, const std::string& ws,
                           const json::Value& connector) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto c = signals::ConnectorConfig::from_json(connector, &err);
  if (!c) return fail(err);
  if (!signals::make_connector(c->provider)) {
    return fail("this build of DevX has no '" + c->provider + "' connector");
  }
  if (!store.save_connector(ws, *c, &err)) return fail(err);
  json::Value o = json::Value::object();
  o.set("connector", c->to_json());
  return ok(std::move(o));
}

json::Value connector_remove(const std::string& sessions_dir, const std::string& ws,
                             const std::string& connector_id, bool delete_local_data) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  if (!store.remove_connector(ws, connector_id, delete_local_data, &err)) return fail(err);
  json::Value o = json::Value::object();
  o.set("local_data_deleted", json::Value::boolean(delete_local_data));
  return ok(std::move(o));
}

json::Value connector_validate(const std::string& sessions_dir, const std::string& ws,
                               const std::string& connector_id, const CancellationToken& cancel) {
  SignalStore store = store_for(sessions_dir);
  signals::RunOptions opts;
  opts.cancel = cancel;
  return signals::validate_connector(store, ws, connector_id, opts);
}

json::Value connector_discover(const std::string& sessions_dir, const std::string& ws,
                               const std::string& connector_id, const CancellationToken& cancel) {
  SignalStore store = store_for(sessions_dir);
  signals::RunOptions opts;
  opts.cancel = cancel;
  return signals::discover_connector(store, ws, connector_id, opts);
}

json::Value sync(const std::string& sessions_dir, const std::string& ws,
                 const std::string& connector_id, const CancellationToken& cancel) {
  SignalStore store = store_for(sessions_dir);
  json::Value e;
  if (!has_workspace(store, ws, &e)) return e;
  signals::RunOptions opts;
  opts.cancel = cancel;
  if (!connector_id.empty()) return signals::sync_connector(store, ws, connector_id, opts);
  json::Value results = json::Value::array();
  bool all_ok = true;
  for (const auto& c : store.connectors(ws)) {
    if (c.paused || cancel.cancelled()) continue;
    json::Value r = signals::sync_connector(store, ws, c.id, opts);
    all_ok = all_ok && r.find("ok") != nullptr && r.find("ok")->as_bool();
    results.push_back(std::move(r));
  }
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(all_ok));
  o.set("results", std::move(results));
  return o;
}

json::Value overview(const std::string& sessions_dir, const std::string& ws) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  json::Value o = correlation::overview(g, signals::integrations(store, ws));
  o.set("storage", store.storage_usage(ws));
  return ok(std::move(o));
}

json::Value signals_query(const std::string& sessions_dir, const std::string& ws, const json::Value& query) {
  SignalStore store = store_for(sessions_dir);
  json::Value e;
  if (!has_workspace(store, ws, &e)) return e;
  json::Value o = store.query(ws, signals::SignalQuery::from_json(query)).to_json();
  o.set("freshness", signals::freshness(store, ws));
  return ok(std::move(o));
}

json::Value signal_read(const std::string& sessions_dir, const std::string& ws, const std::string& id,
                        bool include_raw) {
  SignalStore store = store_for(sessions_dir);
  return correlation::signal_read(store, ws, id, include_raw);
}

json::Value releases(const std::string& sessions_dir, const std::string& ws) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  json::Value o = correlation::releases_json(g);
  o.set("freshness", signals::freshness(store, ws));
  return ok(std::move(o));
}

json::Value release(const std::string& sessions_dir, const std::string& ws, const std::string& key) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  json::Value o = correlation::release_overview(g, key, signals::freshness(store, ws));
  // Pinned or not, for the Releases view.
  const json::Value pins = store.pins(ws);
  bool pinned = false;
  if (const json::Value* a = pins.find("releases"); a != nullptr && a->is_array()) {
    for (const auto& x : a->items()) pinned = pinned || (x.is_string() && x.as_string() == key);
  }
  o.set("pinned", json::Value::boolean(pinned));
  return o;
}

json::Value compare(const std::string& sessions_dir, const std::string& ws, const std::string& base,
                    const std::string& candidate) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  return correlation::release_compare(g, base, candidate);
}

json::Value related(const std::string& sessions_dir, const std::string& ws, const std::string& signal_id) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  return correlation::related(g, signal_id);
}

json::Value code_context(const std::string& sessions_dir, const std::string& ws,
                         const std::string& signal_id) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  return correlation::code_context_for_signal(store, g, ws, signal_id);
}

json::Value evidence_pack(const std::string& sessions_dir, const std::string& ws,
                          const json::Value& scope) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  auto g = correlation::load_graph(store, sessions_dir, ws, &err);
  if (!err.empty()) return fail(err);
  correlation::PackScope s;
  s.release = jstr(scope, "release");
  s.signal_id = jstr(scope, "signal_id");
  s.question = jstr(scope, "question");
  if (const json::Value* v = scope.find("include_raw")) s.include_raw = v->as_bool(true);
  if (const json::Value* v = scope.find("include_code")) s.include_code = v->as_bool(true);
  if (s.release.empty() && s.signal_id.empty()) {
    return fail("an evidence pack is for a release or a signal: name one");
  }
  return ok(correlation::evidence_pack(store, g, ws, s, signals::freshness(store, ws)));
}

json::Value retention(const std::string& sessions_dir, const std::string& ws, bool apply) {
  SignalStore store = store_for(sessions_dir);
  json::Value e;
  if (!has_workspace(store, ws, &e)) return e;
  json::Value o = store.apply_retention(ws, apply, static_cast<std::int64_t>(std::time(nullptr))).to_json();
  o.set("storage", store.storage_usage(ws));
  o.set("pins", store.pins(ws));
  return ok(std::move(o));
}

json::Value pin(const std::string& sessions_dir, const std::string& ws, const std::string& kind,
                const std::string& id, bool pinned) {
  SignalStore store = store_for(sessions_dir);
  json::Value e;
  if (!has_workspace(store, ws, &e)) return e;
  std::string err;
  if (!store.set_pin(ws, kind, id, pinned, &err)) return fail(err);
  json::Value o = json::Value::object();
  o.set("pins", store.pins(ws));
  return ok(std::move(o));
}

json::Value link_session(const std::string& sessions_dir, const std::string& ws,
                         const std::string& session_id, const std::string& release_key, bool linked) {
  SignalStore store = store_for(sessions_dir);
  std::string err;
  if (!correlation::link_session(store, ws, session_id, release_key, linked, &err)) return fail(err);
  return ok(json::Value::object());
}

std::string default_workspace(const std::string& sessions_dir, std::string* error) {
  SignalStore store = store_for(sessions_dir);
  const auto all = store.workspaces();
  if (all.size() == 1) return all[0].id;
  if (error != nullptr) {
    if (all.empty()) {
      *error = "no Intelligence workspace yet: create one with `mpi intelligence workspace create <id>` "
               "or in DevX > Intelligence > Integrations";
    } else {
      std::string names;
      for (const auto& w : all) names += (names.empty() ? "" : ", ") + w.id;
      *error = "several workspaces (" + names + "): name one with --workspace";
    }
  }
  return {};
}

}  // namespace mpi::intelligence
