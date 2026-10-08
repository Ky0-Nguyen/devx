// `mpi intelligence` -- production, CI/CD and other external evidence, kept
// locally and correlated with releases, code and sessions.
//
//   workspaces                                 list workspaces and freshness
//   workspace create <id> [--name N] [--repo PATH] [--apps a,b] [--env production,staging]
//                         [--retention-days N]
//   workspace delete <id>                      removes it and all its local evidence
//   providers                                  connectors this build has
//   integrations                               connectors of a workspace, health, freshness
//   connect <provider> <connector-id> [--setting key=value ...] [--credential-ref SERVICE]
//   disconnect <connector-id> [--delete-local-data]
//   validate <connector-id> | discover <connector-id>
//   sync [<connector-id>]                      every connector that is not paused, by default
//   overview
//   signals [--provider P] [--kind K] [--severity S] [--environment E] [--release KEY]
//           [--version V] [--commit SHA] [--since ISO] [--until ISO] [--text T] [--limit N]
//   signal <signal-id> [--raw]
//   releases | release <key> | compare <base> <candidate>
//   related <signal-id> | code <signal-id>
//   pack (--release KEY | --signal ID) [--question Q]
//   retention [--apply]
//   pin (signal|release) <id> [--unpin]
//   link-session <session-id> <release-key> [--unlink]
//   egress                                     what can leave this Mac
//
// Every command takes --workspace, which may be omitted when there is only
// one. Output is JSON with --json; otherwise a short summary, and JSON for
// the commands whose answer is a document.
#include <iostream>
#include <sstream>

#include "adapters/intelligence/service.hpp"
#include "apps/cli/cli.hpp"

namespace mpi::cli {
namespace {

std::string arg(const Invocation& inv, std::size_t i) {
  return i < inv.positional.size() ? inv.positional[i] : std::string();
}

std::string jstr(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

std::vector<std::string> csv(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream in(s);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

json::Value list_json(const std::vector<std::string>& xs) {
  json::Value a = json::Value::array();
  for (const auto& x : xs) a.push_back(json::Value::string(x));
  return a;
}

// Prints a service answer and turns its "ok" into an exit code.
ExitCode emit(const json::Value& v, bool as_text_summary = false,
              const std::function<void(const json::Value&)>& summary = {}) {
  const json::Value* ok = v.find("ok");
  const bool good = ok == nullptr || ok->as_bool(false);
  if (!good) {
    std::cerr << "error: " << jstr(v, "error") << "\n";
    if (v.find("notes") != nullptr && !v.find("notes")->items().empty()) {
      for (const auto& n : v.find("notes")->items()) std::cerr << "  note: " << n.as_string() << "\n";
    }
    return ExitCode::kCollectionError;
  }
  if (as_text_summary && summary) {
    summary(v);
  } else {
    std::cout << v.dump(2) << "\n";
  }
  return ExitCode::kOk;
}

ExitCode usage() {
  std::cerr << "usage: mpi intelligence <command> [--workspace ID] ...\n"
               "  workspaces | workspace create <id> [--name --repo --apps --env --retention-days]\n"
               "  workspace delete <id> | providers | integrations | egress\n"
               "  connect <provider> <connector-id> [--setting key=value ...] [--credential-ref S]\n"
               "  disconnect <connector-id> [--delete-local-data] | validate <id> | discover <id>\n"
               "  sync [<connector-id>] | overview | signals [filters] | signal <id> [--raw]\n"
               "  releases | release <key> | compare <base> <candidate> | related <id> | code <id>\n"
               "  pack (--release KEY | --signal ID) [--question Q] | retention [--apply]\n"
               "  pin (signal|release) <id> [--unpin] | link-session <session> <release> [--unlink]\n"
               "See docs/intelligence.md.\n";
  return ExitCode::kUsage;
}

}  // namespace

ExitCode cmd_intelligence(const Invocation& inv) {
  const std::string sub = arg(inv, 0);
  const std::string dir = inv.global.sessions_dir;
  const bool text = !inv.global.json;
  if (sub.empty()) return usage();

  if (sub == "providers") return emit(intelligence::providers());
  if (sub == "egress") {
    return emit(intelligence::egress(dir), text, [](const json::Value& v) {
      for (const auto& r : v.find("ledger")->items()) {
        std::cout << "- " << jstr(r, "what") << "\n    to: " << jstr(r, "to") << "\n    when: "
                  << jstr(r, "when") << "\n";
      }
    });
  }
  if (sub == "workspaces") {
    return emit(intelligence::workspaces(dir), text, [](const json::Value& v) {
      const auto& ws = v.find("workspaces")->items();
      if (ws.empty()) std::cout << "no workspaces yet: mpi intelligence workspace create <id>\n";
      for (const auto& w : ws) {
        std::cout << jstr(w, "id") << "  " << jstr(w, "name") << "  repo: "
                  << (jstr(w, "repository_root").empty() ? "-" : jstr(w, "repository_root"))
                  << "  connectors: " << w.find("connectors")->as_int() << "\n";
      }
    });
  }
  if (sub == "workspace") {
    const std::string op = arg(inv, 1), id = arg(inv, 2);
    if (op == "create" && !id.empty()) {
      json::Value w = json::Value::object();
      w.set("id", json::Value::string(id));
      w.set("name", json::Value::string(inv.flag("name", id)));
      w.set("repository_root", json::Value::string(inv.flag("repo")));
      w.set("app_identifiers", list_json(csv(inv.flag("apps"))));
      w.set("environments", list_json(csv(inv.flag("env"))));
      json::Value r = json::Value::object();
      r.set("days", json::Value::integer(std::atoi(inv.flag("retention-days", "30").c_str())));
      w.set("retention", r);
      return emit(intelligence::workspace_save(dir, w));
    }
    if (op == "delete" && !id.empty()) return emit(intelligence::workspace_delete(dir, id));
    return usage();
  }

  // Everything else is about one workspace.
  std::string err;
  std::string ws = inv.flag("workspace");
  if (ws.empty()) ws = intelligence::default_workspace(dir, &err);
  if (ws.empty()) {
    std::cerr << "error: " << err << "\n";
    return ExitCode::kNotFound;
  }

  if (sub == "integrations") {
    return emit(intelligence::integrations(dir, ws), text, [](const json::Value& v) {
      const auto& cs = v.find("connectors")->items();
      if (cs.empty()) std::cout << "no connectors: mpi intelligence connect <provider> <id> --setting k=v\n";
      for (const auto& c : cs) {
        const json::Value& cfg = *c.find("config");
        const json::Value& h = *c.find("health");
        std::cout << jstr(cfg, "id") << "  (" << jstr(cfg, "provider") << ")  " << jstr(h, "state");
        if (!jstr(h, "detail").empty()) std::cout << " -- " << jstr(h, "detail");
        const json::Value* sync = c.find("sync");
        if (sync != nullptr && !jstr(*sync, "last_success_at").empty()) {
          std::cout << "  last success " << jstr(*sync, "last_success_at");
        }
        std::cout << "\n";
      }
    });
  }
  if (sub == "connect") {
    const std::string provider = arg(inv, 1), id = arg(inv, 2);
    if (provider.empty() || id.empty()) return usage();
    json::Value c = json::Value::object();
    c.set("id", json::Value::string(id));
    c.set("provider", json::Value::string(provider));
    c.set("name", json::Value::string(inv.flag("name", id)));
    c.set("credential_ref", json::Value::string(inv.flag("credential-ref")));
    json::Value settings = json::Value::object();
    for (const auto& [k, v] : inv.flags) {
      if (k != "setting") continue;
      const auto eq = v.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::cerr << "error: --setting takes key=value, got '" << v << "'\n";
        return ExitCode::kUsage;
      }
      settings.set(v.substr(0, eq), json::Value::string(v.substr(eq + 1)));
    }
    c.set("settings", settings);
    return emit(intelligence::connector_save(dir, ws, c));
  }
  if (sub == "disconnect" && !arg(inv, 1).empty()) {
    return emit(intelligence::connector_remove(dir, ws, arg(inv, 1), inv.has_flag("delete-local-data")));
  }
  if (sub == "validate" && !arg(inv, 1).empty()) {
    return emit(intelligence::connector_validate(dir, ws, arg(inv, 1), inv.global.cancel));
  }
  if (sub == "discover" && !arg(inv, 1).empty()) {
    return emit(intelligence::connector_discover(dir, ws, arg(inv, 1), inv.global.cancel));
  }
  if (sub == "sync") {
    const json::Value r = intelligence::sync(dir, ws, arg(inv, 1), inv.global.cancel);
    if (!text) {
      std::cout << r.dump(2) << "\n";
    } else {
      std::vector<json::Value> results;
      if (const json::Value* a = r.find("results")) {
        results = a->items();
      } else {
        results.push_back(r);
      }
      if (results.empty()) std::cout << "no connectors to sync\n";
      for (const auto& one : results) {
        std::cout << jstr(one, "connector_id") << ": " << jstr(one, "status") << ", "
                  << (one.find("records_written") ? one.find("records_written")->as_int() : 0)
                  << " record(s), " << (one.find("pages") ? one.find("pages")->as_int() : 0) << " page(s)";
        if (one.find("rate_limited") != nullptr && one.find("rate_limited")->as_bool()) {
          std::cout << ", rate limited";
        }
        if (!jstr(one, "error").empty()) std::cout << "\n  error: " << jstr(one, "error");
        if (const json::Value* n = one.find("notes")) {
          for (const auto& x : n->items()) std::cout << "\n  note: " << x.as_string();
        }
        std::cout << "\n";
      }
    }
    const json::Value* ok = r.find("ok");
    return ok != nullptr && ok->as_bool() ? ExitCode::kOk : ExitCode::kCollectionError;
  }
  if (sub == "overview") return emit(intelligence::overview(dir, ws));
  if (sub == "signals") {
    json::Value q = json::Value::object();
    for (const char* k : {"provider", "kind", "severity", "environment", "version", "commit", "since",
                          "until", "text"}) {
      const std::string v = inv.flag(k);
      if (!v.empty()) q.set(k, json::Value::string(v));
    }
    if (!inv.flag("release").empty()) q.set("release_key", json::Value::string(inv.flag("release")));
    q.set("limit", json::Value::integer(std::atoi(inv.flag("limit", "50").c_str())));
    return emit(intelligence::signals_query(dir, ws, q), text, [](const json::Value& v) {
      const auto& ss = v.find("signals")->items();
      std::cout << v.find("total")->as_int() << " signal(s)"
                << (ss.size() < static_cast<std::size_t>(v.find("total")->as_int()) ? ", newest shown" : "")
                << "\n";
      for (const auto& s : ss) {
        std::cout << "  " << jstr(s, "occurred_at") << "  " << jstr(s, "provider") << "/" << jstr(s, "kind");
        if (!jstr(s, "severity").empty()) std::cout << " [" << jstr(s, "severity") << "]";
        std::cout << "  " << jstr(s, "title");
        if (!jstr(s, "release_key").empty()) std::cout << "  @" << jstr(s, "release_key");
        std::cout << "\n    " << jstr(s, "id") << "\n";
      }
      for (const auto& p : v.find("problems")->items()) std::cout << "  problem: " << p.as_string() << "\n";
    });
  }
  if (sub == "signal" && !arg(inv, 1).empty()) {
    return emit(intelligence::signal_read(dir, ws, arg(inv, 1), inv.has_flag("raw")));
  }
  if (sub == "releases") {
    return emit(intelligence::releases(dir, ws), text, [](const json::Value& v) {
      const auto& rs = v.find("releases")->items();
      if (rs.empty()) std::cout << "no releases: no evidence names one yet\n";
      for (const auto& r : rs) {
        std::cout << jstr(r, "key") << "  crashes " << r.find("crashes")->as_int() << ", issues "
                  << r.find("issues")->as_int() << ", CI failures " << r.find("ci_failures")->as_int()
                  << ", sessions " << r.find("sessions")->as_int();
        if (!jstr(r, "deployed_at").empty()) std::cout << ", deployed " << jstr(r, "deployed_at");
        if (r.find("conflicts")->as_int() > 0) std::cout << ", " << r.find("conflicts")->as_int() << " conflict(s)";
        std::cout << "\n";
      }
    });
  }
  if (sub == "release" && !arg(inv, 1).empty()) return emit(intelligence::release(dir, ws, arg(inv, 1)));
  if (sub == "compare" && !arg(inv, 2).empty()) {
    return emit(intelligence::compare(dir, ws, arg(inv, 1), arg(inv, 2)));
  }
  if (sub == "related" && !arg(inv, 1).empty()) return emit(intelligence::related(dir, ws, arg(inv, 1)));
  if (sub == "code" && !arg(inv, 1).empty()) return emit(intelligence::code_context(dir, ws, arg(inv, 1)));
  if (sub == "pack") {
    json::Value scope = json::Value::object();
    if (!inv.flag("release").empty()) scope.set("release", json::Value::string(inv.flag("release")));
    if (!inv.flag("signal").empty()) scope.set("signal_id", json::Value::string(inv.flag("signal")));
    if (!inv.flag("question").empty()) scope.set("question", json::Value::string(inv.flag("question")));
    return emit(intelligence::evidence_pack(dir, ws, scope));
  }
  if (sub == "retention") return emit(intelligence::retention(dir, ws, inv.has_flag("apply")));
  if (sub == "pin" && !arg(inv, 2).empty()) {
    return emit(intelligence::pin(dir, ws, arg(inv, 1), arg(inv, 2), !inv.has_flag("unpin")));
  }
  if (sub == "link-session" && !arg(inv, 1).empty()) {
    return emit(intelligence::link_session(dir, ws, arg(inv, 1), arg(inv, 2), !inv.has_flag("unlink")));
  }
  return usage();
}

}  // namespace mpi::cli
