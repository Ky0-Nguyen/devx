#include "core/signals/sync.hpp"

#include "core/util/time.hpp"

namespace mpi::signals {
namespace {

json::Value error_json(const std::string& why) {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(false));
  o.set("error", json::Value::string(why));
  return o;
}

// The provider's environment variable belongs to a connector only when it is
// the workspace's one connector of that provider.
bool env_credential_allowed(const SignalStore& store, const std::string& ws, const ConnectorConfig& c) {
  int same = 0;
  for (const auto& other : store.connectors(ws)) same += other.provider == c.provider ? 1 : 0;
  return same <= 1;
}

struct Prepared {
  ConnectorConfig config;
  std::unique_ptr<SignalConnector> connector;
  ConnectorContext ctx;
  std::string error;
};

Prepared prepare(SignalStore& store, const std::string& ws, const std::string& connector_id,
                 const RunOptions& opts) {
  Prepared p;
  auto c = store.connector(ws, connector_id, &p.error);
  if (!c) return p;
  p.config = *c;
  p.connector = make_connector(c->provider);
  if (!p.connector) {
    p.error = "this build of DevX has no '" + c->provider + "' connector";
    return p;
  }
  p.ctx.transport = opts.transport ? opts.transport : net::Transport(net::https_fetch);
  p.ctx.cancel = opts.cancel;
  p.ctx.workspace_id = ws;
  p.ctx.now_iso = time_util::now_iso8601_utc();
  const CredentialSource src = credential_source(*c, p.connector->info(), env_credential_allowed(store, ws, *c));
  if (opts.secret_override) {
    p.ctx.secret = opts.secret_override;
  } else if (src.needed()) {
    p.ctx.secret = net::find_secret(src.env_var, src.keychain_services);
    if (!p.ctx.secret && !p.connector->info().credential_optional) {
      p.error = "no credential for " + c->id + ": " + p.connector->info().credential_help;
    }
  }
  return p;
}

}  // namespace

json::Value validate_connector(SignalStore& store, const std::string& ws,
                               const std::string& connector_id, const RunOptions& opts) {
  Prepared p = prepare(store, ws, connector_id, opts);
  if (!p.error.empty()) return error_json(p.error);
  return p.connector->validate(p.config, p.ctx).to_json();
}

json::Value discover_connector(SignalStore& store, const std::string& ws,
                               const std::string& connector_id, const RunOptions& opts) {
  Prepared p = prepare(store, ws, connector_id, opts);
  if (!p.error.empty()) return error_json(p.error);
  return p.connector->discover(p.config, p.ctx).to_json();
}

json::Value sync_connector(SignalStore& store, const std::string& ws,
                           const std::string& connector_id, const RunOptions& opts) {
  SyncResult r;
  r.started_at = time_util::now_iso8601_utc();
  Prepared p = prepare(store, ws, connector_id, opts);
  if (p.error.empty() && p.config.paused) p.error = connector_id + " is paused";
  if (!p.error.empty()) {
    r.error = p.error;
    r.finished_at = time_util::now_iso8601_utc();
    std::string ignored;
    if (store.connector(ws, connector_id, &ignored)) store.record_sync(ws, connector_id, r, &ignored);
    json::Value o = r.to_json();
    o.set("ok", json::Value::boolean(false));
    o.set("connector_id", json::Value::string(connector_id));
    return o;
  }
  const SyncCursor before = store.cursor(ws, connector_id);
  {
    // The sink flushes the index when it goes out of scope.
    auto sink = store.open_sink(ws, connector_id);
    r = p.connector->sync(p.config, before, *sink, p.ctx);
  }
  if (r.started_at.empty()) r.started_at = p.ctx.now_iso;
  r.finished_at = time_util::now_iso8601_utc();
  r.cursor.connector_id = connector_id;
  r.cursor.updated_at = r.finished_at;
  std::string err;
  // A failed sync keeps the old cursor; a partial one stores where to resume.
  if (r.status != SyncStatus::kFailed && !store.save_cursor(ws, r.cursor, &err)) {
    r.notes.push_back("the cursor was not saved: " + err);
  }
  store.record_sync(ws, connector_id, r, &err);
  json::Value o = r.to_json();
  o.set("ok", json::Value::boolean(r.status != SyncStatus::kFailed));
  o.set("connector_id", json::Value::string(connector_id));
  return o;
}

json::Value integrations(const SignalStore& store, const std::string& ws) {
  json::Value out = json::Value::object();
  std::string err;
  if (!store.workspace(ws, &err)) return error_json(err);
  std::vector<std::string> problems;
  const json::Value status = store.sync_status(ws);
  const json::Value usage = store.storage_usage(ws);
  json::Value list = json::Value::array();
  for (const auto& c : store.connectors(ws, &problems)) {
    json::Value one = json::Value::object();
    one.set("config", c.to_json());
    auto conn = make_connector(c.provider);
    ConnectorHealth h;
    const json::Value* st = status.find(c.id);
    if (conn) {
      one.set("info", conn->info().to_json());
      one.set("capabilities", conn->capabilities().to_json());
      const CredentialSource src = credential_source(c, conn->info(), env_credential_allowed(store, ws, c));
      // Whether a credential exists -- never what it is, and without reading it.
      const bool has_secret = !src.needed() || net::secret_present(src.env_var, src.keychain_services);
      json::Value services = json::Value::array();
      for (const auto& s : src.keychain_services) services.push_back(json::Value::string(s));
      one.set("credential_services", std::move(services));
      one.set("credential_env", json::Value::string(src.env_var));
      one.set("credential_present", json::Value::boolean(has_secret));
      if (c.paused) {
        h.state = "paused";
      } else if (!has_secret && !conn->info().credential_optional) {
        h.state = "needs_auth";
        h.detail = conn->info().credential_help;
      } else if (st == nullptr) {
        h.state = "never_synced";
      } else {
        const json::Value* s = st->find("status");
        const std::string sv = s != nullptr && s->is_string() ? s->as_string() : "";
        h.state = sv == "complete" ? "up_to_date" : sv == "partial" ? "partial" : "error";
        if (const json::Value* e = st->find("error"); e != nullptr && e->is_string()) h.detail = e->as_string();
      }
    } else {
      h.state = "error";
      h.detail = "this build of DevX has no '" + c.provider + "' connector";
    }
    one.set("health", h.to_json());
    one.set("sync", st != nullptr ? *st : json::Value::object());
    one.set("cursor", store.cursor(ws, c.id).to_json());
    list.push_back(std::move(one));
  }
  out.set("ok", json::Value::boolean(true));
  out.set("workspace", json::Value::string(ws));
  out.set("connectors", std::move(list));
  out.set("storage", usage);
  json::Value p = json::Value::array();
  for (const auto& s : problems) p.push_back(json::Value::string(s));
  out.set("problems", std::move(p));
  return out;
}

json::Value freshness(const SignalStore& store, const std::string& ws) {
  json::Value out = json::Value::object();
  const json::Value status = store.sync_status(ws);
  for (const auto& c : store.connectors(ws)) {
    json::Value f = json::Value::object();
    const json::Value* st = status.find(c.id);
    if (st == nullptr) {
      f.set("state", json::Value::string(c.paused ? "paused" : "never_synced"));
    } else {
      for (const char* k : {"status", "last_success_at", "last_attempt_at", "error"}) {
        if (const json::Value* v = st->find(k)) f.set(k == std::string("status") ? "state" : k, *v);
      }
    }
    out.set(c.id, std::move(f));
  }
  return out;
}

json::Value providers() {
  json::Value a = json::Value::array();
  for (const auto& name : registered_providers()) {
    auto c = make_connector(name);
    if (!c) continue;
    json::Value one = c->info().to_json();
    one.set("capabilities", c->capabilities().to_json());
    a.push_back(std::move(one));
  }
  return a;
}

json::Value egress_ledger(const SignalStore& store) {
  json::Value rows = json::Value::array();
  auto row = [&](const std::string& what, const std::string& where, const std::string& when) {
    json::Value r = json::Value::object();
    r.set("what", json::Value::string(what));
    r.set("to", json::Value::string(where));
    r.set("when", json::Value::string(when));
    rows.push_back(std::move(r));
  };
  row("Nothing: captures, sessions, layouts, inspect observations and signals stay on this Mac",
      "-", "always");
  row("The .apk / .aab / .ipa you choose, and API requests with your BrowserStack credentials",
      "BrowserStack (api-cloud / hub-cloud.browserstack.com)",
      "only when you upload, start a session or load BrowserStack data");
  row("Google SDK catalog and package downloads (no data about you or your apps)", "dl.google.com",
      "only when you open Install images or install a package");
  row("Whatever an AI host asks DevX for through `mpi mcp`: by default local evidence only; "
      "--allow-network adds connector refresh, --allow-actions adds device and provider actions",
      "the AI host you configured (Claude, Codex, Cursor), under its own rules",
      "only while that host runs `mpi mcp`");
  for (const auto& w : store.workspaces()) {
    for (const auto& c : store.connectors(w.id)) {
      auto conn = make_connector(c.provider);
      if (!conn || !conn->capabilities().network) continue;
      row(conn->info().egress, conn->info().display_name + " (" + c.id + ", workspace " + w.id + ")",
          c.paused ? "never: paused" : "only when it syncs, validates or discovers");
    }
  }
  json::Value o = json::Value::object();
  o.set("ledger", std::move(rows));
  return o;
}

}  // namespace mpi::signals
