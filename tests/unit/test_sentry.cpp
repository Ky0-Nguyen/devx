// The Sentry connector, driven end to end against replies in the shape of
// Sentry's public API (fixtures/intelligence/sentry, synthetic values).
//
// A fake transport stands in for the network: it answers by URL from the
// fixtures and records every request, so the tests can check what DevX
// asked for, with which header, and where -- and that the token went nowhere
// else.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "adapters/intelligence/builtin.hpp"
#include "adapters/sentry/sentry.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"
#include "core/util/time.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
using mpi::json::Value;
using mpi::net::HttpRequest;
using mpi::net::HttpResponse;
namespace signals = mpi::signals;

const char* kToken = "sntrys_SYNTHETIC_do_not_leak_7f3a9c";
const char* kHost = "sentry.acme.test";
const char* kBase = "https://sentry.acme.test";
const char* kWs = "acme-mobile";
const char* kConn = "sentry-main";
const char* kPage2Cursor = "1759700000000:0:0";

std::string fixture_dir() {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir ? dir : "fixtures") + "/intelligence/sentry/";
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The path of a URL on the fake host, without its query.
std::string path_of(const std::string& url) {
  std::string rest = url.substr(std::string(kBase).size());
  return rest.substr(0, rest.find('?'));
}

// A stand-in for Sentry. `overrides` replaces the reply for a route, once.
struct FakeSentry {
  std::vector<HttpRequest> requests;
  std::map<std::string, HttpResponse> overrides;
  std::string next_link_host = kHost;

  static HttpResponse reply(int status, std::string body) {
    HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
  }

  static std::string route(const std::string& url) {
    if (url.rfind(kBase, 0) != 0) return "elsewhere";
    const std::string p = path_of(url);
    if (p == "/api/0/organizations/acme/") return "org";
    if (p == "/api/0/organizations/acme/projects/") return "projects";
    if (p == "/api/0/projects/acme/mobile-app/") return "project";
    if (p == "/api/0/organizations/acme/releases/") return "releases";
    if (p == "/api/0/projects/acme/mobile-app/issues/") {
      return contains(url, "1759700000000") ? "issues-page2" : "issues-page1";
    }
    const std::string ev = "/api/0/organizations/acme/issues/";
    if (p.rfind(ev, 0) == 0 && p.size() > ev.size() + 15 &&
        p.substr(p.size() - 15) == "/events/latest/") {
      return "event-" + p.substr(ev.size(), p.size() - ev.size() - 15);
    }
    return "unknown";
  }

  HttpResponse answer(const HttpRequest& req) {
    requests.push_back(req);
    const std::string r = route(req.url);
    if (auto it = overrides.find(r); it != overrides.end()) {
      HttpResponse o = it->second;
      overrides.erase(it);
      return o;
    }
    auto file = [&](const std::string& name) {
      auto body = read_file(fixture_dir() + name);
      return body ? reply(200, *body) : reply(404, R"({"detail":"The requested resource does not exist"})");
    };
    if (r == "org") return file("organization.json");
    if (r == "projects") return file("projects.json");
    if (r == "project") return file("project.json");
    if (r == "releases") return file("releases.json");
    if (r == "issues-page1") {
      HttpResponse resp = file("issues-page1.json");
      const std::string next = "https://" + next_link_host +
                               "/api/0/projects/acme/mobile-app/issues/?statsPeriod=14d&cursor=" +
                               kPage2Cursor;
      resp.headers["link"] =
          "<https://sentry.acme.test/api/0/projects/acme/mobile-app/issues/?cursor=0:0:1>; "
          "rel=\"previous\"; results=\"false\"; cursor=\"0:0:1\", <" + next +
          ">; rel=\"next\"; results=\"true\"; cursor=\"" + kPage2Cursor + "\"";
      return resp;
    }
    if (r == "issues-page2") {
      HttpResponse resp = file("issues-page2.json");
      resp.headers["link"] =
          "<https://sentry.acme.test/api/0/projects/acme/mobile-app/issues/?cursor=0:0:1>; "
          "rel=\"previous\"; results=\"true\"; cursor=\"0:0:1\", "
          "<https://sentry.acme.test/api/0/projects/acme/mobile-app/issues/?cursor=1759600000000:0:0>; "
          "rel=\"next\"; results=\"false\"; cursor=\"1759600000000:0:0\"";
      return resp;
    }
    if (r.rfind("event-", 0) == 0) return file(r + "-latest.json");
    return reply(404, R"({"detail":"The requested resource does not exist"})");
  }

  std::vector<const HttpRequest*> to(const std::string& r) const {
    std::vector<const HttpRequest*> out;
    for (const auto& q : requests) {
      if (route(q.url) == r || (r == "issues" && route(q.url).rfind("issues-", 0) == 0)) out.push_back(&q);
    }
    return out;
  }
};

mpi::net::Transport transport_of(const std::shared_ptr<FakeSentry>& fake) {
  return [fake](const HttpRequest& req) { return fake->answer(req); };
}

signals::ConnectorConfig config() {
  signals::ConnectorConfig c;
  c.id = kConn;
  c.provider = "sentry";
  c.name = "Sentry (acme / mobile-app)";
  c.settings.set("base_url", Value::string(kBase));
  c.settings.set("organization", Value::string("acme"));
  c.settings.set("project", Value::string("mobile-app"));
  return c;
}

signals::ConnectorContext context(const std::shared_ptr<FakeSentry>& fake) {
  signals::ConnectorContext ctx;
  ctx.transport = transport_of(fake);
  ctx.secret = mpi::net::Secret{kToken, "environment"};
  ctx.workspace_id = kWs;
  ctx.now_iso = "2026-10-08T01:00:00Z";
  return ctx;
}

// A real store in a fresh temporary directory, with the workspace and the
// connector configured, removed afterwards.
struct Env {
  std::string dir;
  std::unique_ptr<signals::SignalStore> store;
  std::shared_ptr<FakeSentry> fake = std::make_shared<FakeSentry>();

  Env() {
    mpi::intelligence::register_builtin_connectors();
    std::error_code ec;
    std::string pattern = (fs::temp_directory_path(ec) / "devx-sentry-XXXXXX").string();
    std::vector<char> tmpl(pattern.begin(), pattern.end());
    tmpl.push_back('\0');
    const char* made = ::mkdtemp(tmpl.data());
    dir = made != nullptr ? made : "";
    store = std::make_unique<signals::SignalStore>(dir);
    signals::ProjectWorkspace w;
    w.id = kWs;
    w.name = "Acme mobile";
    std::string err;
    MPI_CHECK_MSG(store->save_workspace(w, &err), err);
    MPI_CHECK_MSG(store->save_connector(kWs, config(), &err), err);
  }
  ~Env() {
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
  }

  Value sync() {
    signals::RunOptions o;
    o.transport = transport_of(fake);
    o.secret_override = mpi::net::Secret{kToken, "environment"};
    return signals::sync_connector(*store, kWs, kConn, o);
  }

  std::optional<signals::SignalRecord> signal(const std::string& external_id) {
    std::string err;
    return store->signal(kWs, signals::make_signal_id(kConn, external_id), &err);
  }
};

std::string str_at(const Value& v, const char* key) {
  const Value* f = v.find(key);
  return f != nullptr ? f->as_string() : std::string("<absent>");
}

std::int64_t int_at(const Value& v, const char* key) {
  const Value* f = v.find(key);
  return f != nullptr ? f->as_int(-1) : -1;
}

}  // namespace

MPI_TEST(sentry_validate_names_the_organization, {}) {
  auto fake = std::make_shared<FakeSentry>();
  auto c = mpi::intelligence::make_sentry_connector();
  const auto r = c->validate(config(), context(fake));
  MPI_CHECK_MSG(r.ok, r.error);
  MPI_CHECK_EQ(r.account, std::string("Acme Mobile"));
  MPI_CHECK_EQ(fake->requests.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(fake->requests[0].url, std::string("https://sentry.acme.test/api/0/organizations/acme/"));

  const auto info = c->info();
  MPI_CHECK_EQ(info.credential_env, std::string("SENTRY_AUTH_TOKEN"));
  MPI_CHECK_EQ(info.credential_service, std::string("com.devx.sentry"));
  MPI_CHECK(c->capabilities().incremental_pull && c->capabilities().crashes);
}

MPI_TEST(sentry_validate_reports_a_refused_token_without_echoing_it, {}) {
  auto fake = std::make_shared<FakeSentry>();
  // A reply that quotes the token back must not carry it into the error.
  fake->overrides["org"] = FakeSentry::reply(401, std::string(R"({"detail":"Invalid token: )") + kToken + "\"}");
  auto c = mpi::intelligence::make_sentry_connector();
  const auto r = c->validate(config(), context(fake));
  MPI_CHECK(!r.ok);
  MPI_CHECK_MSG(contains(r.error, "refused the token") && contains(r.error, "401"), r.error);
  MPI_CHECK_MSG(!contains(r.error, kToken), r.error);
  MPI_CHECK(!contains(r.to_json().dump(), kToken));
}

MPI_TEST(sentry_validate_needs_settings_and_https, {}) {
  auto fake = std::make_shared<FakeSentry>();
  auto c = mpi::intelligence::make_sentry_connector();
  auto cfg = config();
  cfg.settings = Value::object();
  cfg.settings.set("organization", Value::string("acme"));
  auto r = c->validate(cfg, context(fake));
  MPI_CHECK_MSG(!r.ok && contains(r.error, "project"), r.error);
  cfg = config();
  cfg.settings.set("base_url", Value::string("http://sentry.acme.test"));
  r = c->validate(cfg, context(fake));
  MPI_CHECK_MSG(!r.ok && contains(r.error, "https"), r.error);
  auto ctx = context(fake);
  ctx.secret.reset();
  r = c->validate(config(), ctx);
  MPI_CHECK_MSG(!r.ok && contains(r.error, "SENTRY_AUTH_TOKEN"), r.error);
  MPI_CHECK_MSG(fake->requests.empty(), "nothing is sent without a usable configuration");
}

MPI_TEST(sentry_discover_lists_projects, {}) {
  auto fake = std::make_shared<FakeSentry>();
  auto c = mpi::intelligence::make_sentry_connector();
  auto cfg = config();
  cfg.settings = Value::object();  // discovery is how a project gets picked
  cfg.settings.set("base_url", Value::string(kBase));
  cfg.settings.set("organization", Value::string("acme"));
  const auto r = c->discover(cfg, context(fake));
  MPI_CHECK_MSG(r.ok, r.error);
  MPI_CHECK_EQ(r.resources.size(), static_cast<std::size_t>(2));
  const Value& first = r.resources.items()[0];
  MPI_CHECK_EQ(str_at(first, "id"), std::string("mobile-app"));
  MPI_CHECK_EQ(str_at(first, "kind"), std::string("project"));
  MPI_CHECK_EQ(str_at(first, "platform"), std::string("apple-ios"));
  MPI_CHECK_EQ(str_at(r.resources.items()[1], "id"), std::string("web-checkout"));
}

MPI_TEST(sentry_full_sync_normalizes_issues_crashes_and_releases, {}) {
  Env env;
  const Value out = env.sync();
  MPI_CHECK_MSG(out.find("ok")->as_bool(), out.dump());
  MPI_CHECK_EQ(str_at(out, "status"), std::string("complete"));
  MPI_CHECK_EQ(int_at(out, "records_written"), 5);  // 2 releases + 3 issues
  MPI_CHECK_EQ(int_at(out, "raw_written"), 7);      // + 2 latest events
  MPI_CHECK_EQ(int_at(out, "pages"), 3);            // releases + 2 issue pages
  MPI_CHECK_MSG(contains(out.find("notes")->dump(), "no latest event"), out.dump());

  // The first issues request reads resolved issues too, within the lookback.
  const auto issues = env.fake->to("issues");
  MPI_CHECK_EQ(issues.size(), static_cast<std::size_t>(2));
  MPI_CHECK_MSG(contains(issues[0]->url, "statsPeriod=14d") &&
                    contains(issues[0]->url, "query=lastSeen%3A-14d"),
                issues[0]->url);
  const auto releases = env.fake->to("releases");
  MPI_CHECK_MSG(contains(releases[0]->url, "project=4504000000000001") &&
                    contains(releases[0]->url, "per_page=50"),
                releases[0]->url);

  // A fatal, unhandled issue is a crash; the others are issues.
  auto crash = env.signal("4970000001");
  MPI_CHECK(crash.has_value());
  MPI_CHECK_EQ(crash->kind, std::string("crash"));
  MPI_CHECK_EQ(crash->severity.value_or(""), std::string("fatal"));
  MPI_CHECK_EQ(crash->occurred_at, std::string("2026-10-02T08:14:03.512000Z"));
  MPI_CHECK_EQ(crash->environment.value_or(""), std::string("production"));
  MPI_CHECK_EQ(crash->release.bundle_or_package_id.value_or(""), std::string("com.acme.app"));
  MPI_CHECK_EQ(crash->release.version.value_or(""), std::string("5.4.0"));
  MPI_CHECK_EQ(crash->release.build_number.value_or(""), std::string("54019"));
  MPI_CHECK_EQ(crash->release.commit_sha.value_or(""),
               std::string("9f2c4e7a1b3d5f6e8a0c2b4d6f8e0a1c3b5d7f9e"));
  MPI_CHECK_EQ(crash->release.key(), std::string("com.acme.app@5.4.0+54019"));
  MPI_CHECK(crash->basis == signals::EvidenceBasis::kProviderAttributed);
  MPI_CHECK(crash->release.source == signals::FactSource::kProvider);
  const Value& a = crash->attributes;
  MPI_CHECK_EQ(int_at(a, "count"), 1423);
  MPI_CHECK_EQ(int_at(a, "user_count"), 611);
  MPI_CHECK_EQ(str_at(a, "short_id"), std::string("MOBILE-APP-3K"));
  MPI_CHECK_EQ(str_at(a, "status"), std::string("unresolved"));
  MPI_CHECK_EQ(str_at(a, "dist"), std::string("54019"));
  MPI_CHECK_EQ(str_at(a, "provider_url"),
               std::string("https://sentry.acme.test/organizations/acme/issues/4970000001/"));
  MPI_CHECK_EQ(str_at(*a.find("exception"), "type"), std::string("NSInvalidArgumentException"));
  // Crash frame first, though Sentry lists the stack oldest call first.
  const Value* frames = a.find("frames");
  MPI_CHECK(frames != nullptr && frames->size() == 5);
  const Value& top = frames->items()[0];
  MPI_CHECK_EQ(str_at(top, "function"), std::string("-[CheckoutViewController pay:]"));
  MPI_CHECK_EQ(str_at(top, "file"), std::string("CheckoutViewController.swift"));
  MPI_CHECK_EQ(int_at(top, "line"), 214);
  MPI_CHECK(top.find("in_app")->as_bool());
  MPI_CHECK_EQ(str_at(frames->items()[1], "function"), std::string("CartFormatter.amountString(_:)"));
  MPI_CHECK_MSG(frames->items()[2].find("file")->is_null(), "a frame with no file says so");
  MPI_CHECK_EQ(str_at(frames->items()[4], "function"), std::string("main"));

  // Raw evidence: the issue as Sentry sent it, byte for byte, and the event.
  auto raw = env.store->read_raw(kWs, crash->raw_ref, 0, 1 << 20);
  MPI_CHECK_MSG(raw.ok, raw.error);
  MPI_CHECK_MSG(contains(raw.bytes, "\"shortId\": \"MOBILE-APP-3K\""), raw.bytes);
  auto event_raw = env.store->read_raw(kWs, str_at(a, "event_raw_ref"), 0, 1 << 20);
  MPI_CHECK_MSG(event_raw.ok, event_raw.error);
  MPI_CHECK_EQ(event_raw.bytes, *read_file(fixture_dir() + "event-4970000001-latest.json"));

  // An issue whose event has no release string: identity from the app
  // context, environment from the tags, stack from the crashed thread.
  auto handled = env.signal("4970000002");
  MPI_CHECK(handled.has_value());
  MPI_CHECK_EQ(handled->kind, std::string("issue"));
  MPI_CHECK_EQ(handled->severity.value_or(""), std::string("error"));
  MPI_CHECK_EQ(handled->environment.value_or(""), std::string("staging"));
  MPI_CHECK_EQ(handled->release.key(), std::string("com.acme.app@5.3.2+53011"));
  MPI_CHECK_MSG(!handled->release.commit_sha, "no commit is invented for a release Sentry gave none");
  MPI_CHECK_EQ(str_at(handled->attributes, "status"), std::string("resolved"));
  MPI_CHECK_EQ(str_at(handled->attributes.find("frames")->items()[0], "function"),
               std::string("HTTPClient.send(_:)"));

  // No latest event: no release, and the basis says there is no link.
  auto quiet = env.signal("4970000003");
  MPI_CHECK(quiet.has_value());
  MPI_CHECK_EQ(quiet->severity.value_or(""), std::string("warning"));
  MPI_CHECK(quiet->release.empty());
  MPI_CHECK(quiet->basis == signals::EvidenceBasis::kUnknown);
  MPI_CHECK(!quiet->environment.has_value());

  // Releases.
  auto rel = env.signal("release:com.acme.app@5.4.0+54019");
  MPI_CHECK(rel.has_value());
  MPI_CHECK_EQ(rel->kind, std::string("release"));
  MPI_CHECK_EQ(rel->occurred_at, std::string("2026-10-01T09:00:00.000000Z"));
  MPI_CHECK_EQ(rel->release.commit_sha.value_or(""), std::string("9f2c4e7a1b3d5f6e8a0c2b4d6f8e0a1c3b5d7f9e"));
  MPI_CHECK_EQ(rel->release.environment.value_or(""), std::string("production"));
  MPI_CHECK_EQ(int_at(rel->attributes, "new_groups"), 3);
  MPI_CHECK_EQ(int_at(rel->attributes, "commit_count"), 12);
  MPI_CHECK_EQ(str_at(*rel->attributes.find("last_deploy"), "finished_at"),
               std::string("2026-10-01T09:00:00.000000Z"));
  MPI_CHECK(env.store->read_raw(kWs, rel->raw_ref, 0, 1 << 20).ok);
  auto older = env.signal("release:com.acme.app@5.3.2+53011");
  MPI_CHECK(older.has_value());
  MPI_CHECK_EQ(older->occurred_at, std::string("2026-09-18T07:10:00.000000Z"));  // never released
  MPI_CHECK(!older->release.commit_sha && older->attributes.find("last_deploy") == nullptr);

  // A complete sync moves the watermark to the newest lastSeen.
  const auto cur = env.store->cursor(kWs, kConn);
  MPI_CHECK_EQ(cur.watermark, std::string("2026-10-07T21:40:11.004000Z"));
  MPI_CHECK(cur.resume.empty());
}

MPI_TEST(sentry_follows_link_pagination, {}) {
  Env env;
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("complete"));
  const auto issues = env.fake->to("issues");
  MPI_CHECK_EQ(issues.size(), static_cast<std::size_t>(2));
  // Page two is the URL Sentry's Link header named, not one DevX built.
  MPI_CHECK_EQ(issues[1]->url, std::string("https://sentry.acme.test/api/0/projects/acme/mobile-app/"
                                           "issues/?statsPeriod=14d&cursor=") + kPage2Cursor);
  MPI_CHECK(env.signal("4970000003").has_value());
}

MPI_TEST(sentry_rate_limit_keeps_page_one_and_resumes, {}) {
  Env env;
  HttpResponse limited = FakeSentry::reply(429, R"({"detail":"Request was throttled."})");
  limited.headers["retry-after"] = "30";
  env.fake->overrides["issues-page2"] = limited;
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("partial"));
  MPI_CHECK(out.find("ok")->as_bool());
  MPI_CHECK(out.find("rate_limited")->as_bool());
  MPI_CHECK_EQ(int_at(out, "retry_after_s"), 30);
  MPI_CHECK_MSG(contains(str_at(out, "error"), "429"), out.dump());
  MPI_CHECK_EQ(int_at(out, "records_written"), 4);  // 2 releases + page one
  MPI_CHECK(env.signal("4970000001").has_value());
  MPI_CHECK(env.signal("4970000002").has_value());
  MPI_CHECK(!env.signal("4970000003").has_value());
  auto cur = env.store->cursor(kWs, kConn);
  MPI_CHECK_MSG(cur.watermark.empty(), "a partial sync never advances the watermark");
  MPI_CHECK_EQ(cur.resume, std::string(kPage2Cursor));

  // The next sync starts where the last one stopped.
  env.fake->requests.clear();
  const Value again = env.sync();
  MPI_CHECK_EQ(str_at(again, "status"), std::string("complete"));
  const auto issues = env.fake->to("issues");
  MPI_CHECK_EQ(issues.size(), static_cast<std::size_t>(1));
  MPI_CHECK_MSG(contains(issues[0]->url, "cursor=1759700000000%3A0%3A0"), issues[0]->url);
  MPI_CHECK(env.signal("4970000003").has_value());
  cur = env.store->cursor(kWs, kConn);
  MPI_CHECK(cur.resume.empty());
  // Only what this sync saw counts: page one is read again next time,
  // which costs a page and loses nothing.
  MPI_CHECK_EQ(cur.watermark, std::string("2026-10-05T07:45:30.000000Z"));
}

MPI_TEST(sentry_rate_limit_before_anything_is_written_fails, {}) {
  Env env;
  HttpResponse limited = FakeSentry::reply(429, "{}");
  limited.headers["retry-after"] = "12";
  env.fake->overrides["project"] = limited;
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("failed"));
  MPI_CHECK(out.find("rate_limited")->as_bool());
  MPI_CHECK_EQ(int_at(out, "retry_after_s"), 12);
  MPI_CHECK_EQ(int_at(out, "records_written"), 0);
}

MPI_TEST(sentry_incremental_sync_asks_for_what_changed, {}) {
  Env env;
  signals::SyncCursor c;
  c.connector_id = kConn;
  c.watermark = "2026-10-06T00:00:00.250000Z";
  // A full read happened recently, so this one is incremental.
  c.extra.set("last_full_at", Value::string(mpi::time_util::now_iso8601_utc()));
  std::string err;
  MPI_CHECK_MSG(env.store->save_cursor(kWs, c, &err), err);
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("complete"));
  const auto issues = env.fake->to("issues");
  MPI_CHECK(!issues.empty());
  // lastSeen:>2026-10-06T00:00:00Z, fraction dropped so nothing is skipped.
  MPI_CHECK_MSG(contains(issues[0]->url, "query=lastSeen%3A%3E2026-10-06T00%3A00%3A00Z"), issues[0]->url);
  MPI_CHECK_MSG(!contains(issues[0]->url, "unresolved"), "resolved issues are read too");
  MPI_CHECK_EQ(env.store->cursor(kWs, kConn).watermark, std::string("2026-10-07T21:40:11.004000Z"));
}

MPI_TEST(sentry_token_goes_only_into_the_authorization_header, {}) {
  Env env;
  // A 401 that echoes the token, then a full sync.
  env.fake->overrides["org"] = FakeSentry::reply(401, std::string(R"({"detail":")") + kToken + "\"}");
  signals::RunOptions o;
  o.transport = transport_of(env.fake);
  o.secret_override = mpi::net::Secret{kToken, "environment"};
  const Value v = signals::validate_connector(*env.store, kWs, kConn, o);
  MPI_CHECK(!v.find("ok")->as_bool());
  MPI_CHECK(!contains(v.dump(), kToken));
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("complete"));
  MPI_CHECK(!contains(out.dump(), kToken));

  MPI_CHECK(!env.fake->requests.empty());
  for (const auto& req : env.fake->requests) {
    MPI_CHECK_MSG(!contains(req.url, kToken), req.url);
    MPI_CHECK(req.allowed_hosts == std::vector<std::string>{kHost});
    int auth = 0;
    for (const auto& [name, value] : req.headers) {
      if (name == "Authorization") {
        auth++;
        MPI_CHECK_EQ(value, std::string("Bearer ") + kToken);
      } else {
        MPI_CHECK(!contains(value, kToken));
      }
    }
    MPI_CHECK_EQ(auth, 1);
  }
  int files = 0;
  for (const auto& e : fs::recursive_directory_iterator(env.dir)) {
    if (!e.is_regular_file()) continue;
    files++;
    const auto bytes = read_file(e.path().string());
    MPI_CHECK_MSG(bytes && !contains(*bytes, kToken), e.path().string());
  }
  MPI_CHECK(files > 10);
  std::vector<std::string> problems;
  for (const auto& r : env.store->all_signals(kWs, &problems)) {
    MPI_CHECK(!contains(r.to_json().dump(), kToken));
  }
  MPI_CHECK(problems.empty());
}

MPI_TEST(sentry_bounds_and_foreign_links_stop_with_a_resume_point, {}) {
  // The connector alone, with a sink that keeps records in memory.
  struct MemorySink : signals::SignalSink {
    std::vector<signals::SignalRecord> records;
    std::string put_raw(const std::string& p, const std::string& c, const std::string& n,
                        const std::string&) override {
      return "raw/" + p + "/" + c + "/" + n;
    }
    bool put_signal(signals::SignalRecord r, std::string*) override {
      records.push_back(std::move(r));
      return true;
    }
  };
  auto c = mpi::intelligence::make_sentry_connector();
  {
    auto fake = std::make_shared<FakeSentry>();
    auto ctx = context(fake);
    ctx.max_pages = 1;
    MemorySink sink;
    const auto r = c->sync(config(), signals::SyncCursor{}, sink, ctx);
    MPI_CHECK(r.status == signals::SyncStatus::kPartial);
    MPI_CHECK_EQ(r.cursor.resume, std::string(kPage2Cursor));
    MPI_CHECK(r.cursor.watermark.empty());
    MPI_CHECK_EQ(sink.records.size(), static_cast<std::size_t>(4));
  }
  {
    // A next link to another host is never followed: the token would go
    // with it.
    auto fake = std::make_shared<FakeSentry>();
    fake->next_link_host = "collector.elsewhere.test";
    MemorySink sink;
    const auto r = c->sync(config(), signals::SyncCursor{}, sink, context(fake));
    MPI_CHECK(r.status == signals::SyncStatus::kPartial);
    for (const auto& req : fake->requests) MPI_CHECK_MSG(contains(req.url, kBase), req.url);
    bool noted = false;
    for (const auto& n : r.notes) noted = noted || contains(n, "pointed away");
    MPI_CHECK(noted);
  }
  {
    // max_latest_events bounds the event reads and says so.
    auto fake = std::make_shared<FakeSentry>();
    auto cfg = config();
    cfg.settings.set("max_latest_events", Value::integer(1));
    MemorySink sink;
    const auto r = c->sync(cfg, signals::SyncCursor{}, sink, context(fake));
    MPI_CHECK(r.status == signals::SyncStatus::kComplete);
    MPI_CHECK_EQ(fake->to("event-4970000001").size(), static_cast<std::size_t>(1));
    MPI_CHECK(fake->to("event-4970000002").empty());
    bool noted = false;
    for (const auto& n : r.notes) noted = noted || contains(n, "max_latest_events");
    MPI_CHECK(noted);
  }
}

MPI_TEST(sentry_release_strings_parse_by_convention, {}) {
  using mpi::intelligence::parse_sentry_release;
  auto plain = parse_sentry_release("5.4.0");
  MPI_CHECK_EQ(plain.version.value_or(""), std::string("5.4.0"));
  MPI_CHECK(!plain.build_number && !plain.bundle_or_package_id);
  MPI_CHECK(plain.source == signals::FactSource::kProvider);

  auto pkg = parse_sentry_release("pkg@1.2");
  MPI_CHECK_EQ(pkg.bundle_or_package_id.value_or(""), std::string("pkg"));
  MPI_CHECK_EQ(pkg.version.value_or(""), std::string("1.2"));
  MPI_CHECK(!pkg.build_number);

  auto full = parse_sentry_release("pkg@1.2+77");
  MPI_CHECK_EQ(full.bundle_or_package_id.value_or(""), std::string("pkg"));
  MPI_CHECK_EQ(full.version.value_or(""), std::string("1.2"));
  MPI_CHECK_EQ(full.build_number.value_or(""), std::string("77"));
  MPI_CHECK_EQ(full.key(), std::string("pkg@1.2+77"));

  auto scoped = parse_sentry_release("@acme/web@3.1.0");
  MPI_CHECK_EQ(scoped.bundle_or_package_id.value_or(""), std::string("@acme/web"));
  MPI_CHECK_EQ(scoped.version.value_or(""), std::string("3.1.0"));

  // Without a package the convention does not apply: no build is read in.
  auto plain_build = parse_sentry_release("5.4.0+77");
  MPI_CHECK_EQ(plain_build.version.value_or(""), std::string("5.4.0+77"));
  MPI_CHECK(!plain_build.build_number);

  auto sha = parse_sentry_release("9F2C4E7A1B3D5F6E8A0C2B4D6F8E0A1C3B5D7F9E");
  MPI_CHECK_EQ(sha.commit_sha.value_or(""), std::string("9f2c4e7a1b3d5f6e8a0c2b4d6f8e0a1c3b5d7f9e"));
  MPI_CHECK(!sha.version);
  MPI_CHECK_MSG(parse_sentry_release("1234567").version.has_value(),
                "a short hex string is a version, not a guessed commit");
  MPI_CHECK(parse_sentry_release("").empty());
}

MPI_TEST(sentry_reads_the_whole_window_once_a_day_for_state_changes, {}) {
  // An issue resolved without a new event keeps its lastSeen, so an
  // incremental query never returns it. With no full read in the last day
  // the sync reads the whole lookback window, and records when it did.
  Env env;
  signals::SyncCursor c;
  c.connector_id = kConn;
  c.watermark = "2026-10-06T00:00:00Z";
  c.extra.set("last_full_at", Value::string("2026-01-01T00:00:00Z"));
  std::string err;
  MPI_CHECK_MSG(env.store->save_cursor(kWs, c, &err), err);
  const Value out = env.sync();
  MPI_CHECK_EQ(str_at(out, "status"), std::string("complete"));
  const auto issues = env.fake->to("issues");
  MPI_CHECK(!issues.empty());
  MPI_CHECK_MSG(contains(issues[0]->url, "lastSeen%3A-"), issues[0]->url);
  const auto after = env.store->cursor(kWs, kConn);
  const Value* last = after.extra.find("last_full_at");
  MPI_CHECK(last != nullptr && last->as_string() != "2026-01-01T00:00:00Z");
}

MPI_TEST(sentry_keeps_what_an_earlier_event_said_when_it_reads_none, {}) {
  // A first sync reads the latest events; a later one, with no event reads
  // left, must not strip the release and stack it learnt the first time.
  Env env;
  MPI_CHECK_EQ(str_at(env.sync(), "status"), std::string("complete"));
  const auto before = env.signal("4970000001");
  MPI_CHECK(before.has_value() && before->release.version.has_value());
  auto cfg = config();
  cfg.settings.set("max_latest_events", Value::integer(0));
  std::string err;
  MPI_CHECK_MSG(env.store->save_connector(kWs, cfg, &err), err);
  MPI_CHECK_EQ(str_at(env.sync(), "status"), std::string("complete"));
  const auto after = env.signal("4970000001");
  MPI_CHECK(after.has_value());
  MPI_CHECK_EQ(after->release.key(), before->release.key());
  MPI_CHECK(after->basis == before->basis);
  MPI_CHECK(after->attributes.find("frames") != nullptr);
  MPI_CHECK(after->attributes.find("event_from_earlier_sync") != nullptr);
}
