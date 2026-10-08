// Correlation and what an agent is handed (ADR-0012, section 16).
//
// The precedence is the product: an exact commit beats a short prefix, which
// is only a candidate; a session joins a release exactly only when app,
// version and build all agree; timing alone is a candidate and never more;
// two sources that disagree about a release leave a visible conflict. The
// evidence pack is checked for its bounds and its redaction, and code
// context runs against a real git repository made in the test.
#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/correlation/engine.hpp"
#include "core/correlation/evidence.hpp"
#include "core/signals/signal_store.hpp"
#include "core/util/process.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
using namespace mpi;
using namespace mpi::signals;
using namespace mpi::correlation;

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/devx-corr-XXXXXX";
    path = ::mkdtemp(tmpl);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

SignalRecord sig(const std::string& provider, const std::string& kind, const std::string& ext,
                 const std::string& when) {
  SignalRecord r;
  r.provider = provider;
  r.connector_id = provider + "-main";
  r.kind = kind;
  r.external_id = ext;
  r.id = make_signal_id(r.connector_id, ext);
  r.occurred_at = when;
  r.observed_at = when;
  return r;
}

ReleaseIdentity rel(const char* version, const char* build, const char* commit) {
  ReleaseIdentity r;
  if (version != nullptr) r.version = version;
  if (build != nullptr) r.build_number = build;
  if (commit != nullptr) r.commit_sha = commit;
  if (version != nullptr) r.bundle_or_package_id = "com.acme.app";
  r.source = FactSource::kProvider;
  return r;
}

const std::string kSha = "18ab9120c0ffee18ab9120c0ffee18ab9120c0ff";

bool has_edge(const std::vector<CorrelationEdge>& edges, const std::string& from, const std::string& to,
              EvidenceBasis basis) {
  for (const auto& e : edges) {
    if (e.from_id == from && e.to_id == to && e.basis == basis) return true;
  }
  return false;
}

Inputs fixture_inputs() {
  Inputs in;
  in.workspace.id = "superapp";
  in.workspace.app_identifiers = {"com.acme.app"};
  // Sentry's release record names the commit; its issue names the release.
  auto release = sig("sentry", "release", "release:5.4.0", "2026-10-08T00:40:00Z");
  release.release = rel("5.4.0", "54019", kSha.c_str());
  release.basis = EvidenceBasis::kProviderAttributed;
  auto crash = sig("sentry", "crash", "8921", "2026-10-08T01:15:44Z");
  crash.release = rel("5.4.0", "54019", nullptr);
  crash.basis = EvidenceBasis::kProviderAttributed;
  crash.severity = "fatal";
  crash.environment = "production";
  crash.title = "OutOfMemoryError";
  crash.attributes.set("count", json::Value::string("1200"));
  crash.attributes.set("user_count", json::Value::integer(324));
  // GitLab built and deployed that commit.
  auto pipeline = sig("gitlab", "pipeline", "pipeline:9321", "2026-10-08T00:10:00Z");
  pipeline.release.commit_sha = kSha;
  pipeline.release.branch = "release/5.4.0";
  pipeline.basis = EvidenceBasis::kExact;
  pipeline.attributes.set("status", json::Value::string("success"));
  auto deploy = sig("gitlab", "deploy", "deploy:77", "2026-10-08T00:53:00Z");
  deploy.release.commit_sha = kSha;
  deploy.environment = "production";
  deploy.basis = EvidenceBasis::kExact;
  deploy.attributes.set("status", json::Value::string("success"));
  // A job on a different commit that shares only a 7-character prefix.
  auto other = sig("gitlab", "ci_job", "job:5", "2026-10-07T10:00:00Z");
  other.release.commit_sha = "18ab912";
  other.severity = "error";
  other.basis = EvidenceBasis::kExact;
  // A production issue that names no release at all.
  auto orphan = sig("firebase", "issue", "anon", "2026-10-08T01:30:00Z");
  orphan.environment = "production";
  in.signals = {release, crash, pipeline, deploy, other, orphan};

  SessionRef exact;
  exact.id = "s-1";
  exact.app_identifier = "com.acme.app";
  exact.version = "5.4.0";
  exact.build = "54019";
  exact.created_at = "2026-10-08T02:00:00Z";
  SessionRef no_build = exact;
  no_build.id = "s-2";
  no_build.build.reset();
  SessionRef other_app = exact;
  other_app.id = "s-3";
  other_app.app_identifier = "com.other";
  in.sessions = {exact, no_build, other_app};
  return in;
}

}  // namespace

MPI_TEST(exact_identity_joins_ci_and_production_to_one_release, {}) {
  const Graph g = build_graph(fixture_inputs());
  const std::string key = "com.acme.app@5.4.0+54019";
  MPI_CHECK(g.releases.count(key) == 1);
  const Release& r = g.releases.at(key);
  MPI_CHECK_EQ(*r.identity.commit_sha, kSha);
  MPI_CHECK_EQ(*r.deployed_at, std::string("2026-10-08T00:53:00Z"));
  MPI_CHECK(has_edge(g.edges, make_signal_id("gitlab-main", "pipeline:9321"), "release:" + key, EvidenceBasis::kExact));
  MPI_CHECK(has_edge(g.edges, make_signal_id("gitlab-main", "deploy:77"), "release:" + key, EvidenceBasis::kExact));
  MPI_CHECK(has_edge(g.edges, make_signal_id("sentry-main", "8921"), "release:" + key,
                     EvidenceBasis::kProviderAttributed));
}

MPI_TEST(a_short_prefix_is_only_a_candidate_and_exact_outranks_it, {}) {
  const Graph g = build_graph(fixture_inputs());
  const std::string job = make_signal_id("gitlab-main", "job:5");
  MPI_CHECK(has_edge(g.candidate_edges, job, "release:com.acme.app@5.4.0+54019", EvidenceBasis::kCandidate));
  MPI_CHECK_MSG(!has_edge(g.edges, job, "release:com.acme.app@5.4.0+54019", EvidenceBasis::kExact),
                "a prefix is never filed as exact");
  // The pipeline matched exactly, so it carries no candidate edge at all.
  const std::string pipeline = make_signal_id("gitlab-main", "pipeline:9321");
  for (const auto& e : g.candidate_edges) MPI_CHECK(e.from_id != pipeline);
}

MPI_TEST(sessions_join_exactly_only_on_app_version_and_build, {}) {
  const Graph g = build_graph(fixture_inputs());
  const std::string node = "release:com.acme.app@5.4.0+54019";
  MPI_CHECK(has_edge(g.edges, node, "session:s-1", EvidenceBasis::kExact));
  MPI_CHECK(has_edge(g.candidate_edges, node, "session:s-2", EvidenceBasis::kCandidate));
  MPI_CHECK_MSG(g.sessions.count("s-3") == 0, "another app's session is not this workspace's");
  Inputs in = fixture_inputs();
  in.session_links["s-2"] = "com.acme.app@5.4.0+54019";
  const Graph linked = build_graph(in);
  MPI_CHECK_MSG(has_edge(linked.edges, node, "session:s-2", EvidenceBasis::kProviderAttributed),
                "a user's link is stated as theirs, not as exact");
}

MPI_TEST(timing_alone_is_a_candidate_never_a_cause, {}) {
  const Graph g = build_graph(fixture_inputs());
  const std::string orphan = make_signal_id("firebase-main", "anon");
  bool found = false;
  for (const auto& e : g.candidate_edges) {
    if (e.from_id == orphan) {
      found = true;
      MPI_CHECK(e.basis == EvidenceBasis::kCandidate);
      MPI_CHECK(e.evidence.find("timing only") != std::string::npos);
      MPI_CHECK(e.evidence.find("37 min after deploy") != std::string::npos);
    }
  }
  MPI_CHECK(found);
  for (const auto& e : g.edges) MPI_CHECK(e.from_id != orphan);
  // And a candidate never comes out of the JSON as exact.
  const json::Value ov = release_overview(g, "com.acme.app@5.4.0+54019", json::Value::object());
  for (const auto& e : ov.find("exact_links")->items()) {
    MPI_CHECK(e.find("basis")->as_string() != "candidate");
  }
}

MPI_TEST(disagreeing_sources_leave_a_visible_conflict, {}) {
  Inputs in = fixture_inputs();
  auto liar = sig("firebase", "crash", "x", "2026-10-08T03:00:00Z");
  liar.release = rel("5.4.0", "54019", "0000000000000000000000000000000000000000");
  in.signals.push_back(liar);
  const Graph g = build_graph(in);
  const Release& r = g.releases.at("com.acme.app@5.4.0+54019");
  MPI_CHECK_EQ(r.conflicts.size(), std::size_t{1});
  MPI_CHECK(r.conflicts[0].find("commit_sha") != std::string::npos);
}

MPI_TEST(a_release_overview_says_what_is_missing, {}) {
  Inputs in = fixture_inputs();
  in.sessions.clear();
  const Graph g = build_graph(in);
  const json::Value ov = release_overview(g, "com.acme.app@5.4.0+54019", json::Value::object());
  MPI_CHECK(ov.find("ok")->as_bool());
  const std::string missing = ov.find("missing_evidence")->dump();
  MPI_CHECK(missing.find("no DevX or BrowserStack session") != std::string::npos);
  MPI_CHECK(ov.find("timeline")->items().size() >= 3);
  MPI_CHECK(ov.find("evidence")->find("gitlab") != nullptr);
  MPI_CHECK(!release_overview(g, "nope", json::Value::object()).find("ok")->as_bool());
}

MPI_TEST(release_compare_reports_deltas_without_inventing_them, {}) {
  Inputs in = fixture_inputs();
  auto m1 = sig("firebase", "metric", "perf:a", "2026-10-01T00:00:00Z");
  m1.release = rel("5.3.0", "53000", nullptr);
  m1.attributes.set("event_type", json::Value::string("DURATION_TRACE"));
  m1.attributes.set("event_name", json::Value::string("_app_start"));
  m1.attributes.set("unit", json::Value::string("ms"));
  m1.attributes.set("p95", json::Value::number(1900));
  auto m2 = m1;
  m2.external_id = "perf:b";
  m2.id = make_signal_id(m2.connector_id, m2.external_id);
  m2.release = rel("5.4.0", "54019", nullptr);
  m2.attributes.set("p95", json::Value::number(3100));
  m2.attributes.set("p50", json::Value::number(900));  // no p50 in base: no delta
  in.signals.push_back(m1);
  in.signals.push_back(m2);
  const Graph g = build_graph(in);
  const json::Value c = release_compare(g, "com.acme.app@5.3.0+53000", "com.acme.app@5.4.0+54019");
  MPI_CHECK(c.find("ok")->as_bool());
  const auto& deltas = c.find("metric_deltas")->items();
  MPI_CHECK_EQ(deltas.size(), std::size_t{1});
  MPI_CHECK(deltas[0].find("p95")->find("delta")->as_double() == 1200);
  MPI_CHECK_MSG(deltas[0].find("p50") == nullptr, "a percentile missing on one side has no delta");
  MPI_CHECK_EQ(c.find("candidate_summary")->find("crashes")->as_int(), std::int64_t{1});
  MPI_CHECK(c.find("candidate_summary")->find("affected_users")->as_double() == 324);
  MPI_CHECK_MSG(c.find("base_summary")->find("affected_users") == nullptr,
                "no crash reported a user count for the base: absent, not zero");
}

MPI_TEST(an_evidence_pack_is_bounded_cited_and_redacted, {}) {
  TempDir t;
  SignalStore store(t.path);
  ProjectWorkspace w;
  w.id = "superapp";
  w.app_identifiers = {"com.acme.app"};
  std::string err;
  store.save_workspace(w, &err);
  Inputs in = fixture_inputs();
  // A failed job whose log carries a token; the excerpt must not.
  auto job = sig("gitlab", "ci_job", "job:91821", "2026-10-08T00:20:00Z");
  job.release.commit_sha = kSha;
  job.severity = "error";
  job.basis = EvidenceBasis::kExact;
  std::string log;
  for (int i = 1; i <= 5000; i++) log += "noise line " + std::to_string(i) + "\n";
  log += "export TOKEN=glpat-ABCDEFGHIJKLMNOPQRSTUV\n> Task :app:mergeProdNativeLibs FAILED\n";
  job.raw_ref = store.put_raw("superapp", "gitlab", "jobs", "91821.log", log);
  job.attributes.set("root_error", json::Value::string("mergeProdNativeLibs failed"));
  job.attributes.set("excerpt_start", json::Value::integer(4990));
  job.attributes.set("excerpt_end", json::Value::integer(5002));
  in.signals.push_back(job);
  for (const auto& s : in.signals) MPI_CHECK_MSG(store.put_signal("superapp", s, &err), err);
  const Graph g = load_graph(store, t.path, "superapp", &err);
  PackScope scope;
  scope.release = "com.acme.app@5.4.0+54019";
  scope.question = "why did 5.4.0 regress?";
  const json::Value pack = evidence_pack(store, g, "superapp", scope, json::Value::object());
  const std::string text = pack.dump();
  MPI_CHECK(text.size() <= kMaxPackBytes);
  MPI_CHECK_MSG(text.find("glpat-") == std::string::npos, "a token in a log never reaches a model");
  MPI_CHECK(text.find("mergeProdNativeLibs FAILED") != std::string::npos);
  MPI_CHECK_MSG(text.find("noise line 100\\n") == std::string::npos, "only the located window, not the log");
  MPI_CHECK(pack.find("source_refs")->items().size() >= 3);
  MPI_CHECK(pack.find("exact_links")->items().size() >= 3);
  MPI_CHECK(pack.find("question_scope")->find("question")->as_string() == "why did 5.4.0 regress?");
  MPI_CHECK(!pack.find("missing_evidence")->items().empty());

  const json::Value read = signal_read(store, "superapp", job.id, true);
  MPI_CHECK(read.find("raw")->find("text")->as_string().find("[redacted:gitlab_token]") != std::string::npos);
  const json::Value plain = signal_read(store, "superapp", job.id, false);
  MPI_CHECK_MSG(plain.find("raw") == nullptr, "raw only when asked for");
}

MPI_TEST(code_context_resolves_frames_at_the_release_commit, {}) {
  TempDir t;
  const std::string repo = t.path + "/repo";
  fs::create_directories(repo + "/src/cache");
  auto git = [&](std::vector<std::string> args) {
    std::vector<std::string> argv = {"/usr/bin/git", "-C", repo, "-c", "user.name=Dev", "-c",
                                     "user.email=dev@example.com", "-c", "commit.gpgsign=false"};
    argv.insert(argv.end(), args.begin(), args.end());
    return proc::run(argv);
  };
  MPI_CHECK(git({"init", "-q"}).ok());
  std::string body;
  for (int i = 1; i <= 100; i++) body += "line " + std::to_string(i) + "\n";
  std::ofstream(repo + "/src/cache/VideoCache.ts") << body;
  MPI_CHECK(git({"add", "-A"}).ok());
  MPI_CHECK(git({"commit", "-q", "-m", "base"}).ok());
  body.replace(body.find("line 84\n"), 8, "allocate(huge) // line 84\n");
  std::ofstream(repo + "/src/cache/VideoCache.ts") << body;
  MPI_CHECK(git({"commit", "-qam", "bigger video cache"}).ok());
  const std::string head = [&] {
    auto r = git({"rev-parse", "HEAD"});
    std::string s = r.out;
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
  }();

  SignalStore store(t.path);
  ProjectWorkspace w;
  w.id = "superapp";
  w.repository_root = repo;
  std::string err;
  store.save_workspace(w, &err);
  auto crash = sig("sentry", "crash", "8921", "2026-10-08T01:15:44Z");
  crash.release = rel("5.4.0", "54019", head.c_str());
  crash.basis = EvidenceBasis::kProviderAttributed;
  json::Value frames = json::Value::array();
  json::Value f = json::Value::object();
  f.set("file", json::Value::string("webpack:///./src/cache/VideoCache.ts"));
  f.set("line", json::Value::integer(84));
  f.set("in_app", json::Value::boolean(true));
  frames.push_back(f);
  json::Value outside = json::Value::object();
  outside.set("file", json::Value::string("../../etc/passwd"));
  outside.set("line", json::Value::integer(1));
  frames.push_back(outside);
  crash.attributes.set("frames", frames);
  store.put_signal("superapp", crash, &err);
  const Graph g = load_graph(store, t.path, "superapp", &err);
  const json::Value ctx = code_context_for_signal(store, g, "superapp", crash.id);
  MPI_CHECK_MSG(ctx.find("ok")->as_bool(), ctx.dump());
  MPI_CHECK(ctx.find("ref_basis")->as_string().rfind("exact", 0) == 0);
  const auto& resolved = ctx.find("frames")->items();
  MPI_CHECK_EQ(resolved[0].find("path")->as_string(), std::string("src/cache/VideoCache.ts"));
  MPI_CHECK(resolved[0].find("source")->find("text")->as_string().find("84  allocate(huge)") != std::string::npos);
  MPI_CHECK(resolved[0].find("blame")->find("summary")->as_string() == "bigger video cache");
  MPI_CHECK_MSG(resolved[0].find("blame")->find("author")->as_string() == "Dev" &&
                    ctx.dump().find("dev@example.com") == std::string::npos,
                "blame names the author, never their e-mail");
  MPI_CHECK_MSG(!resolved[1].find("resolved")->as_bool(), "a path outside the repository is not read");
  MPI_CHECK(ctx.find("diff")->find("patch")->as_string().find("+allocate(huge)") != std::string::npos);

  // The repository moved: code context is unavailable, nothing else breaks.
  fs::rename(repo, t.path + "/moved");
  const json::Value gone = code_context_for_signal(store, g, "superapp", crash.id);
  MPI_CHECK(!gone.find("ok")->as_bool());
  MPI_CHECK(gone.find("error")->as_string().find("moved") != std::string::npos);
  MPI_CHECK(store.query("superapp", SignalQuery{}).total == 1);
}
