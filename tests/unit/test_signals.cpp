// The signal plane's contracts and its local store (ADR-0008, ADR-0009).
//
// The store is the system of record for Intelligence, so its failure modes
// are tested as carefully as its happy path: a sync that dies halfway, a
// corrupt record, raw evidence re-synced with different bytes, retention
// with pins, a connector removed with its evidence kept. The last tests add
// a connector from outside core -- the extensibility promise (AC-12) -- and
// run it through the same sync path every front end uses.
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/net/https_fetch.hpp"
#include "core/signals/connector.hpp"
#include "core/signals/redact.hpp"
#include "core/signals/signal.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
using namespace mpi;
using namespace mpi::signals;

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/devx-signals-XXXXXX";
    path = ::mkdtemp(tmpl);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

SignalRecord crash(const std::string& ext, const std::string& when) {
  SignalRecord r;
  r.provider = "sentry";
  r.connector_id = "sentry-main";
  r.kind = "crash";
  r.external_id = ext;
  r.occurred_at = when;
  r.observed_at = when;
  r.severity = "fatal";
  r.title = "OutOfMemoryError " + ext;
  r.release.version = "5.4.0";
  r.release.build_number = "54019";
  r.release.bundle_or_package_id = "com.acme.app";
  r.release.source = FactSource::kProvider;
  r.basis = EvidenceBasis::kProviderAttributed;
  return r;
}

SignalStore make_store(const TempDir& t, const std::string& ws = "superapp") {
  SignalStore store(t.path);
  ProjectWorkspace w;
  w.id = ws;
  w.name = "Super App";
  w.app_identifiers = {"com.acme.app"};
  std::string err;
  MPI_CHECK_MSG(store.save_workspace(w, &err), err);
  return store;
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

MPI_TEST(a_signal_round_trips_and_absent_stays_absent, {}) {
  SignalRecord r = crash("8921", "2026-10-08T01:15:44Z");
  r.id = make_signal_id(r.connector_id, r.external_id);
  r.release.commit_sha = "18AB912";  // read back lowercased
  std::string err;
  auto back = SignalRecord::from_json(r.to_json(), &err);
  MPI_CHECK_MSG(back.has_value(), err);
  MPI_CHECK_EQ(back->id, std::string("sig_sentry-main_8921"));
  MPI_CHECK_EQ(*back->release.commit_sha, std::string("18ab912"));
  MPI_CHECK_MSG(!back->release.branch.has_value(), "an absent branch is absent, not an empty string");
  MPI_CHECK(!back->environment.has_value());
  MPI_CHECK_EQ(back->release.key(), std::string("com.acme.app@5.4.0+54019"));

  ReleaseIdentity only_commit;
  only_commit.commit_sha = "18ab912";
  MPI_CHECK_EQ(only_commit.key(), std::string("commit:18ab912"));
  MPI_CHECK_MSG(ReleaseIdentity{}.key().empty(), "no identity, no release invented for it");

  json::Value bad = r.to_json();
  bad.set("schema_version", json::Value::string("devx.signal/9"));
  MPI_CHECK(!SignalRecord::from_json(bad, &err).has_value());
  MPI_CHECK(err.find("schema") != std::string::npos);
}

MPI_TEST(connector_settings_refuse_secrets_and_ids_are_safe, {}) {
  json::Value c = json::Value::object();
  c.set("id", json::Value::string("sentry-main"));
  c.set("provider", json::Value::string("sentry"));
  json::Value settings = json::Value::object();
  settings.set("organization", json::Value::string("acme"));
  settings.set("auth_token", json::Value::string("sntrys_abc"));
  c.set("settings", settings);
  std::string err;
  MPI_CHECK_MSG(!ConnectorConfig::from_json(c, &err).has_value(), "a secret-looking setting is refused");
  MPI_CHECK(err.find("auth_token") != std::string::npos);
  MPI_CHECK(!id_is_safe("../etc"));
  MPI_CHECK(!id_is_safe("-x"));
  MPI_CHECK(id_is_safe("gitlab_ci-2"));
  MPI_CHECK(looks_like_sha("18ab912"));
  MPI_CHECK(!looks_like_sha("18AB912"));
}

MPI_TEST(iso8601_parses_the_forms_providers_send, {}) {
  MPI_CHECK_EQ(*parse_iso8601("1970-01-01T00:00:00Z"), std::int64_t{0});
  MPI_CHECK_EQ(*parse_iso8601("2026-10-08T01:15:44Z"), std::int64_t{1791422144});
  MPI_CHECK_EQ(*parse_iso8601("2026-10-08T08:15:44.123+07:00"), std::int64_t{1791422144});
  MPI_CHECK_EQ(*parse_iso8601("2026-10-08 01:15:44.123456 UTC"), std::int64_t{1791422144});
  MPI_CHECK_EQ(*parse_iso8601("2026-10-08"), std::int64_t{1791417600});
  MPI_CHECK(!parse_iso8601("yesterday").has_value());
  MPI_CHECK_EQ(*parse_iso8601("2026-10-08T08:45:44+0730"), std::int64_t{1791422144});
  MPI_CHECK_EQ(*parse_iso8601("2026-10-07T20:15:44-05"), std::int64_t{1791422144});
  MPI_CHECK_MSG(!parse_iso8601("2026-10-08 01:15:44 PST").has_value(), "an unknown zone is refused, not read as UTC");
  MPI_CHECK(!parse_iso8601("2026-10-08xyz").has_value());
  MPI_CHECK_EQ(format_iso8601(1791422144), std::string("2026-10-08T01:15:44Z"));
}

MPI_TEST(the_store_keeps_signals_and_answers_queries_offline, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  for (int i = 0; i < 5; i++) {
    SignalRecord r = crash(std::to_string(100 + i), "2026-10-0" + std::to_string(i + 1) + "T10:00:00Z");
    if (i == 4) {
      r.kind = "issue";
      r.severity = "warning";
      r.release = ReleaseIdentity{};
      r.basis = EvidenceBasis::kUnknown;
    }
    MPI_CHECK_MSG(store.put_signal("superapp", r, &err), err);
  }
  SignalQuery q;
  auto all = store.query("superapp", q);
  MPI_CHECK_EQ(all.total, std::size_t{5});
  MPI_CHECK_MSG(all.entries.front().find("occurred_at")->as_string() == "2026-10-05T10:00:00Z",
                "newest first");
  q.kind = "crash";
  MPI_CHECK_EQ(store.query("superapp", q).total, std::size_t{4});
  q = SignalQuery{};
  q.release_key = "com.acme.app@5.4.0+54019";
  MPI_CHECK_EQ(store.query("superapp", q).total, std::size_t{4});
  q = SignalQuery{};
  q.since = "2026-10-03T00:00:00Z";
  MPI_CHECK_EQ(store.query("superapp", q).total, std::size_t{3});
  q = SignalQuery{};
  q.text = "outofmemoryerror 102";
  MPI_CHECK_EQ(store.query("superapp", q).total, std::size_t{1});

  // Files are owner-only.
  struct stat st {};
  ::stat((t.path + "/intelligence/superapp/signals/2026-10-01/sig_sentry-main_100.json").c_str(), &st);
  MPI_CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  ::stat((t.path + "/intelligence/superapp").c_str(), &st);
  MPI_CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0700);
  store.put_raw("superapp", "gitlab", "jobs", "1.log", "x");
  for (const char* d : {"/intelligence/superapp/signals", "/intelligence/superapp/raw",
                        "/intelligence/superapp/raw/gitlab", "/intelligence/superapp/raw/gitlab/jobs"}) {
    ::stat((t.path + d).c_str(), &st);
    MPI_CHECK_MSG((st.st_mode & 0777) == 0700, std::string("owner-only, not just the leaf: ") + d);
  }
}

MPI_TEST(an_update_moves_the_record_and_never_duplicates_it, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  store.put_signal("superapp", crash("7", "2026-10-01T10:00:00Z"), &err);
  SignalRecord moved = crash("7", "2026-10-02T10:00:00Z");
  moved.title = "renamed";
  store.put_signal("superapp", moved, &err);
  auto all = store.all_signals("superapp");
  MPI_CHECK_EQ(all.size(), std::size_t{1});
  MPI_CHECK_EQ(*all[0].title, std::string("renamed"));
}

MPI_TEST(a_sync_that_died_halfway_is_rebuilt_not_trusted, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  {
    auto sink = store.open_sink("superapp", "sentry-main");
    sink->put_signal(crash("1", "2026-10-01T10:00:00Z"), &err);
  }  // flushed: index clean
  MPI_CHECK(!fs::exists(t.path + "/intelligence/superapp/state/index-dirty"));
  // A second sync writes a record and "dies" before flushing: simulate by
  // writing through put_signal (marks dirty) and corrupting the index.
  store.put_signal("superapp", crash("2", "2026-10-02T10:00:00Z"), &err);
  std::ofstream(t.path + "/intelligence/superapp/signals/index.jsonl") << "{not json\n";
  auto r = store.query("superapp", SignalQuery{});
  MPI_CHECK_MSG(r.total == 2, "the dirty marker made the reader rebuild from the records");
}

MPI_TEST(a_corrupt_record_is_quarantined_and_named, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  store.put_signal("superapp", crash("1", "2026-10-01T10:00:00Z"), &err);
  store.put_signal("superapp", crash("2", "2026-10-01T11:00:00Z"), &err);
  std::ofstream(t.path + "/intelligence/superapp/signals/2026-10-01/sig_sentry-main_2.json") << "{\"half\":";
  std::vector<std::string> problems;
  auto all = store.all_signals("superapp", &problems);
  MPI_CHECK_EQ(all.size(), std::size_t{1});
  MPI_CHECK_EQ(problems.size(), std::size_t{1});
  MPI_CHECK(problems[0].find("quarantined") != std::string::npos);
  MPI_CHECK(fs::exists(t.path + "/intelligence/superapp/quarantine/2026-10-01-sig_sentry-main_2.json"));
  MPI_CHECK_EQ(store.storage_usage("superapp").find("quarantined")->as_int(), std::int64_t{1});
}

MPI_TEST(raw_evidence_is_immutable_and_reads_are_confined, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  const std::string a = store.put_raw("superapp", "gitlab", "jobs", "91821.log", "line 1\nline 2\n");
  MPI_CHECK_EQ(a, std::string("raw/gitlab/jobs/91821.log"));
  MPI_CHECK_EQ(store.put_raw("superapp", "gitlab", "jobs", "91821.log", "line 1\nline 2\n"), a);
  const std::string b = store.put_raw("superapp", "gitlab", "jobs", "91821.log", "different\n");
  MPI_CHECK_EQ(b, std::string("raw/gitlab/jobs/91821.v2.log"));
  MPI_CHECK_EQ(store.read_raw("superapp", a, 0, 1024).bytes, std::string("line 1\nline 2\n"));
  auto part = store.read_raw("superapp", a, 2, 4);
  MPI_CHECK_EQ(part.bytes, std::string("ne 1"));
  MPI_CHECK(part.truncated);
  MPI_CHECK(!store.read_raw("superapp", "raw/../workspace.json", 0, 10).ok);
  MPI_CHECK(!store.read_raw("superapp", "/etc/passwd", 0, 10).ok);
  MPI_CHECK(!store.read_raw("superapp", "connectors/x.json", 0, 10).ok);
  MPI_CHECK_MSG(store.put_raw("superapp", "../x", "y", "z", "q").find("..") == std::string::npos,
                "a provider name cannot climb out of raw/");
}

MPI_TEST(retention_expires_old_evidence_but_keeps_pins, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  SignalRecord old = crash("1", "2026-08-01T10:00:00Z");
  old.raw_ref = store.put_raw("superapp", "sentry", "issues", "1.json", "{}");
  store.put_signal("superapp", old, &err);
  SignalRecord pinned = crash("2", "2026-08-01T10:00:00Z");
  // A pinned record's evidence lives in its attributes too (Sentry's event,
  // Firebase's other export files): all of it stays.
  pinned.raw_ref = store.put_raw("superapp", "sentry", "issues", "2.json", "{}");
  pinned.attributes.set("event_raw_ref",
                        json::Value::string(store.put_raw("superapp", "sentry", "events", "2.json", "{}")));
  json::Value more = json::Value::array();
  more.push_back(json::Value::string(store.put_raw("superapp", "firebase", "exports", "b.jsonl", "{}")));
  pinned.attributes.set("raw_refs", more);
  store.put_signal("superapp", pinned, &err);
  // Raw written today is inside the window even with no record naming it.
  const std::string fresh = store.put_raw("superapp", "gitlab", "jobs", "fresh.log", "x");
  // Everything else was written "two months ago".
  for (const auto& e : fs::recursive_directory_iterator(t.path + "/intelligence/superapp/raw")) {
    if (e.is_regular_file() && e.path().filename() != "fresh.log") {
      struct timeval tv[2] = {{1786000000, 0}, {1786000000, 0}};
      ::utimes(e.path().c_str(), tv);
    }
  }
  store.put_signal("superapp", crash("3", "2026-10-07T10:00:00Z"), &err);
  store.set_pin("superapp", "signal", make_signal_id("sentry-main", "2"), true, &err);
  const auto now = *parse_iso8601("2026-10-08T00:00:00Z");

  auto dry = store.apply_retention("superapp", false, now);
  MPI_CHECK(!dry.applied);
  MPI_CHECK_EQ(dry.signals_expired, 1);
  MPI_CHECK_EQ(dry.kept_by_pin, 1);
  MPI_CHECK_EQ(store.all_signals("superapp").size(), std::size_t{3});  // nothing deleted

  auto done = store.apply_retention("superapp", true, now);
  MPI_CHECK_EQ(done.signals_expired, 1);
  MPI_CHECK_EQ(done.raw_deleted, 1);
  MPI_CHECK_EQ(store.all_signals("superapp").size(), std::size_t{2});
  MPI_CHECK(!store.read_raw("superapp", "raw/sentry/issues/1.json", 0, 10).ok);
  for (const char* kept : {"raw/sentry/issues/2.json", "raw/sentry/events/2.json", "raw/firebase/exports/b.jsonl"}) {
    MPI_CHECK_MSG(store.read_raw("superapp", kept, 0, 10).ok, std::string("a pin keeps ") + kept);
  }
  MPI_CHECK_MSG(store.read_raw("superapp", fresh, 0, 10).ok, "raw inside the window is kept");

  ProjectWorkspace forever = *store.workspace("superapp", &err);
  forever.retention.days = 0;
  store.save_workspace(forever, &err);
  MPI_CHECK_EQ(store.apply_retention("superapp", true, now + 365 * 86400).signals_expired, 0);
}

MPI_TEST(removing_a_connector_keeps_its_evidence_unless_asked, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  ConnectorConfig c;
  c.id = "sentry-main";
  c.provider = "sentry";
  MPI_CHECK_MSG(store.save_connector("superapp", c, &err), err);
  store.put_signal("superapp", crash("1", "2026-10-01T10:00:00Z"), &err);
  MPI_CHECK(store.remove_connector("superapp", "sentry-main", false, &err));
  MPI_CHECK_MSG(store.all_signals("superapp").size() == 1, "AC-10: disconnecting deletes nothing local");
  MPI_CHECK(store.connectors("superapp").empty());
  store.save_connector("superapp", c, &err);
  MPI_CHECK(store.remove_connector("superapp", "sentry-main", true, &err));
  MPI_CHECK(store.all_signals("superapp").empty());
}

MPI_TEST(redaction_removes_known_secret_shapes, {}) {
  const std::string log =
      "curl -H 'Authorization: Bearer abcdefghijklmnop123' https://x\n"
      "export GITLAB=glpat-ABCDEFGHIJKLMNOPQRST\n"
      "password=hunter22 user=ci@example.com from 10.0.0.12\n"
      "AKIAABCDEFGHIJKLMNOP and https://bot:s3cr3t@git.example.com/repo.git\n"
      "version 5.4.0 build 54019\n";
  const Redaction r = redact(log);
  MPI_CHECK(r.text.find("abcdefghijklmnop123") == std::string::npos);
  MPI_CHECK(r.text.find("glpat-") == std::string::npos);
  MPI_CHECK(r.text.find("hunter22") == std::string::npos);
  MPI_CHECK(r.text.find("ci@example.com") == std::string::npos);
  MPI_CHECK(r.text.find("10.0.0.12") == std::string::npos);
  MPI_CHECK(r.text.find("AKIAABCDEFGHIJKLMNOP") == std::string::npos);
  MPI_CHECK(r.text.find("s3cr3t") == std::string::npos);
  MPI_CHECK_MSG(r.text.find("Authorization: Bearer [redacted:bearer_token]") != std::string::npos,
                "the excerpt still reads");
  MPI_CHECK_MSG(r.text.find("version 5.4.0 build 54019") != std::string::npos, "ordinary text survives");
  MPI_CHECK(r.total() >= 7);
  MPI_CHECK_MSG(redact("release 1.2.3.4", true, false).text == "release 1.2.3.4",
                "a four-part version is left alone where IPs are off");
}

MPI_TEST(https_helpers_follow_only_allowed_hosts, {}) {
  MPI_CHECK_EQ(net::url_host("https://Sentry.io:443/api/0/"), std::string("sentry.io"));
  MPI_CHECK(net::url_host("http://sentry.io/").empty());
  MPI_CHECK_MSG(net::url_host("https://user:pw@sentry.io/").empty(), "credentials in a URL are refused");
  MPI_CHECK(net::url_allowed("https://sentry.io/api/0/x", {"sentry.io"}));
  MPI_CHECK(!net::url_allowed("https://evil.example/api", {"sentry.io"}));
  const std::string link =
      "<https://sentry.io/api/0/x/?cursor=0:0:1>; rel=\"previous\"; results=\"false\"; cursor=\"0:0:1\", "
      "<https://sentry.io/api/0/x/?cursor=0:100:0>; rel=\"next\"; results=\"true\"; cursor=\"0:100:0\"";
  MPI_CHECK_EQ(*net::next_link(link), std::string("https://sentry.io/api/0/x/?cursor=0:100:0"));
  MPI_CHECK(!net::next_link("<https://s/x>; rel=\"next\"; results=\"false\"").has_value());
  MPI_CHECK_EQ(net::url_encode("lastSeen:>2026"), std::string("lastSeen%3A%3E2026"));
  net::HttpResponse r;
  r.headers["retry-after"] = "30";
  MPI_CHECK_EQ(*net::retry_after_seconds(r), 30);
  net::HttpRequest req;
  req.url = "https://evil.example/";
  req.allowed_hosts = {"sentry.io"};
  MPI_CHECK_MSG(!net::https_fetch(req).ok, "refused before curl runs");
}

// ---- a connector from outside core ----

namespace {

class FixtureConnector : public SignalConnector {
 public:
  ConnectorInfo info() const override {
    ConnectorInfo i;
    i.provider = "fixture";
    i.display_name = "Fixture";
    i.credential_env = "DEVX_TEST_FIXTURE_TOKEN";
    i.credential_service = "com.devx.test-fixture-never-exists";
    return i;
  }
  ConnectorCapabilities capabilities() const override { return {}; }
  ValidateResult validate(const ConnectorConfig&, const ConnectorContext& ctx) override {
    ValidateResult r;
    r.ok = ctx.secret.has_value();
    return r;
  }
  DiscoverResult discover(const ConnectorConfig&, const ConnectorContext&) override { return {}; }
  SyncResult sync(const ConnectorConfig& config, const SyncCursor& cursor, SignalSink& sink,
                  const ConnectorContext& ctx) override {
    SyncResult r;
    seen_secret = ctx.secret ? ctx.secret->value : "";
    const int start = cursor.resume.empty() ? 0 : std::atoi(cursor.resume.c_str());
    for (int i = start; i < 4; i++) {
      if (i == fail_at) {
        r.status = SyncStatus::kPartial;
        r.cursor.resume = std::to_string(i);
        r.cursor.watermark = cursor.watermark;
        return r;
      }
      SignalRecord s;
      s.provider = "fixture";
      s.connector_id = config.id;
      s.kind = "event";
      s.external_id = "e" + std::to_string(i);
      s.occurred_at = "2026-10-0" + std::to_string(i + 1) + "T00:00:00Z";
      s.observed_at = ctx.now_iso;
      s.raw_ref = sink.put_raw("fixture", "events", s.external_id + ".json", "{\"i\":" + std::to_string(i) + "}");
      std::string err;
      if (sink.put_signal(s, &err)) r.records_written++;
    }
    r.status = SyncStatus::kComplete;
    r.cursor.watermark = "2026-10-04T00:00:00Z";
    return r;
  }
  static inline int fail_at = -1;
  static inline std::string seen_secret;
};

}  // namespace

MPI_TEST(a_new_connector_needs_no_change_in_core, {}) {
  register_connector("fixture", [] { return std::make_unique<FixtureConnector>(); });
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  ConnectorConfig c;
  c.id = "fixture-1";
  c.provider = "fixture";
  store.save_connector("superapp", c, &err);

  // No credential: refused before the connector runs.
  auto none = sync_connector(store, "superapp", "fixture-1", RunOptions{});
  MPI_CHECK(!none.find("ok")->as_bool());
  MPI_CHECK(none.find("error")->as_string().find("no credential") != std::string::npos);

  RunOptions opts;
  opts.secret_override = net::Secret{"tok-123456", "test"};
  FixtureConnector::fail_at = 2;
  auto partial = sync_connector(store, "superapp", "fixture-1", opts);
  MPI_CHECK_EQ(partial.find("status")->as_string(), std::string("partial"));
  MPI_CHECK_EQ(store.all_signals("superapp").size(), std::size_t{2});
  MPI_CHECK_MSG(store.cursor("superapp", "fixture-1").watermark.empty(),
                "a partial sync does not advance the watermark");
  auto status = store.sync_status("superapp");
  MPI_CHECK(status.find("fixture-1")->find("last_success_at") == nullptr);

  FixtureConnector::fail_at = -1;
  auto done = sync_connector(store, "superapp", "fixture-1", opts);
  MPI_CHECK_EQ(done.find("status")->as_string(), std::string("complete"));
  MPI_CHECK_EQ(store.all_signals("superapp").size(), std::size_t{4});
  MPI_CHECK_EQ(store.cursor("superapp", "fixture-1").watermark, std::string("2026-10-04T00:00:00Z"));
  MPI_CHECK(store.sync_status("superapp").find("fixture-1")->find("last_success_at") != nullptr);
  MPI_CHECK_EQ(FixtureConnector::seen_secret, std::string("tok-123456"));

  // The secret reached the connector and nothing else.
  for (const auto& e : fs::recursive_directory_iterator(t.path)) {
    if (e.is_regular_file()) {
      MPI_CHECK_MSG(slurp(e.path().string()).find("tok-123456") == std::string::npos,
                    "AC-03: the credential is in no stored file");
    }
  }
  // Integrations reports it with no secret value.
  auto ints = integrations(store, "superapp");
  MPI_CHECK(ints.dump().find("tok-123456") == std::string::npos);
  MPI_CHECK_EQ(ints.find("connectors")->items().size(), std::size_t{1});
}

MPI_TEST(superseded_raw_versions_are_pruned_unless_cited, {}) {
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  // v1 is cited by a pinned-looking older record; v2 by nothing; v3 is newest.
  const std::string v1 = store.put_raw("superapp", "sentry", "issues", "9.json", "one");
  const std::string v2 = store.put_raw("superapp", "sentry", "issues", "9.json", "two");
  SignalRecord keeps = crash("old", "2026-10-01T00:00:00Z");
  keeps.attributes.set("event_raw_ref", json::Value::string(v1));
  store.put_signal("superapp", keeps, &err);
  {
    auto sink = store.open_sink("superapp", "sentry-main");
    MPI_CHECK_EQ(sink->put_raw("sentry", "issues", "9.json", "three"), std::string("raw/sentry/issues/9.v3.json"));
    MPI_CHECK_MSG(sink->put_raw("sentry", "issues", "9.json", "three") == "raw/sentry/issues/9.v3.json",
                  "same bytes as the newest: same ref");
  }  // the sink prunes on close
  MPI_CHECK(store.read_raw("superapp", v1, 0, 10).ok);
  MPI_CHECK_MSG(!store.read_raw("superapp", v2, 0, 10).ok, "superseded and uncited: pruned");
  MPI_CHECK(store.read_raw("superapp", "raw/sentry/issues/9.v3.json", 0, 10).ok);
  // A new version after a gap is numbered past the newest, never into it.
  MPI_CHECK_EQ(store.put_raw("superapp", "sentry", "issues", "9.json", "four"),
               std::string("raw/sentry/issues/9.v4.json"));
  auto lines = store.read_raw_lines("superapp", store.put_raw("superapp", "gitlab", "jobs", "l.log", "a\nb\nc\nd\n"),
                                    2, 3, 100);
  MPI_CHECK_EQ(lines.bytes, std::string("b\nc\n"));
}

MPI_TEST(signal_ids_never_collide_by_sanitising, {}) {
  MPI_CHECK_EQ(make_signal_id("gitlab-main", "pipeline:9321"), std::string("sig_gitlab-main_pipeline_9321"));
  MPI_CHECK_EQ(make_signal_id("sentry-main", "8921"), std::string("sig_sentry-main_8921"));
  MPI_CHECK(make_signal_id("jsonl", "ABC-1") != make_signal_id("jsonl", "abc-1"));
  MPI_CHECK(make_signal_id("sentry", "release:1.2.0+45") != make_signal_id("sentry", "release:1.2.0_45"));
  const std::string a(100, 'a');
  MPI_CHECK(make_signal_id("x", a + "1") != make_signal_id("x", a + "2"));
  MPI_CHECK(make_signal_id("x", a + "1").size() <= 64);
  MPI_CHECK(id_is_safe(make_signal_id("jsonl", "Weird Id / with spaces")));
  MPI_CHECK_EQ(make_signal_id("jsonl", "ABC-1"), make_signal_id("jsonl", "ABC-1"));
}

MPI_TEST(a_credential_belongs_to_one_connector, {}) {
  ConnectorInfo info;
  info.credential_env = "DEVX_TEST_FIXTURE_TOKEN";
  info.credential_service = "com.devx.fixture";
  ConnectorConfig c;
  c.id = "prod";
  c.provider = "fixture";
  auto s = credential_source(c, info, /*env_allowed=*/true);
  MPI_CHECK_EQ(s.env_var, std::string("DEVX_TEST_FIXTURE_TOKEN"));
  MPI_CHECK_EQ(s.keychain_services.size(), std::size_t{2});
  MPI_CHECK_EQ(s.keychain_services[0], std::string("com.devx.fixture.prod"));
  MPI_CHECK_MSG(credential_source(c, info, false).env_var.empty(),
                "with two connectors of a provider, its env var belongs to neither");
  c.credential_ref = "com.acme.sentry-eu";
  auto r = credential_source(c, info, true);
  MPI_CHECK_EQ(r.keychain_services.size(), std::size_t{1});
  MPI_CHECK_EQ(r.keychain_services[0], std::string("com.acme.sentry-eu"));
  MPI_CHECK(!credential_source(c, ConnectorInfo{}, true).needed());

  // End to end: an env token reaches a lone connector, not one of two.
  register_connector("fixture", [] { return std::make_unique<FixtureConnector>(); });
  TempDir t;
  SignalStore store = make_store(t);
  std::string err;
  ConnectorConfig one;
  one.id = "fixture-1";
  one.provider = "fixture";
  store.save_connector("superapp", one, &err);
  ::setenv("DEVX_TEST_FIXTURE_TOKEN", "env-token-1", 1);
  FixtureConnector::fail_at = -1;
  auto ok = sync_connector(store, "superapp", "fixture-1", RunOptions{});
  MPI_CHECK(ok.find("ok")->as_bool());
  MPI_CHECK_EQ(FixtureConnector::seen_secret, std::string("env-token-1"));
  ConnectorConfig two = one;
  two.id = "fixture-2";
  store.save_connector("superapp", two, &err);
  auto refused = sync_connector(store, "superapp", "fixture-2", RunOptions{});
  MPI_CHECK_MSG(!refused.find("ok")->as_bool(), "two connectors: the env token is not handed to either");
  ::unsetenv("DEVX_TEST_FIXTURE_TOKEN");
}

