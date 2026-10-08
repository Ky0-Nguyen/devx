// GitLab CI: the job-log failure finder, and the connector driven end to end
// through signals::sync_connector against recorded (synthetic) replies.
//
// The fixtures are hand-written in GitLab REST v4 / Runner 17 shapes; see
// fixtures/intelligence/gitlab/README.md. The fake transport below is the
// only thing standing in for GitLab: everything from URL building to the
// records on disk is the production code.
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "adapters/gitlab/ci_log.hpp"
#include "adapters/gitlab/gitlab.hpp"
#include "adapters/intelligence/builtin.hpp"
#include "core/signals/signal_store.hpp"
#include "core/signals/sync.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

namespace fs = std::filesystem;
namespace sig = mpi::signals;
using mpi::intelligence::FailureSummary;
using mpi::intelligence::find_failure;

const char* const kToken = "glpat-SYNTHETIC-test-token-0000";
const char* const kBase = "https://gitlab.example.com";

std::string fixture_path(const std::string& name) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return std::string(dir != nullptr ? dir : "fixtures") + "/intelligence/gitlab/" + name;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string fixture(const std::string& name) { return read_file(fixture_path(name)); }

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// A field, or null: a missing field fails the check that reads it rather
// than crashing the whole run.
const mpi::json::Value& field(const mpi::json::Value& v, const char* key) {
  static const mpi::json::Value kNull;
  const auto* f = v.find(key);
  return f != nullptr ? *f : kNull;
}

// A check the rest of a test depends on: record the failure and stop.
#define REQUIRE(cond)  \
  do {                 \
    if (!(cond)) {     \
      MPI_CHECK(cond); \
      return;          \
    }                  \
  } while (false)

// ---- a fake GitLab ----

std::string query_param(const std::string& query, const std::string& key) {
  std::size_t pos = 0;
  while (pos <= query.size()) {
    const std::size_t amp = query.find('&', pos);
    const std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    if (part.rfind(key + "=", 0) == 0) return part.substr(key.size() + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

struct FakeGitLab {
  std::vector<mpi::net::HttpRequest> requests;
  /// Consulted first: a reply to inject (a 429), or nullopt for the fixtures.
  std::function<std::optional<mpi::net::HttpResponse>(const std::string& path, const std::string& query)> inject;

  static mpi::net::HttpResponse reply(int status, std::string body, std::string next_page = "-") {
    mpi::net::HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    if (next_page != "-") r.headers["x-next-page"] = next_page;
    return r;
  }
  static mpi::net::HttpResponse file(const std::string& name, std::string next_page = "-") {
    const std::string body = fixture(name);
    if (body.empty()) return reply(404, R"({"message":"404 Not Found"})");
    return reply(200, body, std::move(next_page));
  }

  mpi::net::HttpResponse handle(const mpi::net::HttpRequest& req) {
    requests.push_back(req);
    const std::string prefix = std::string(kBase) + "/api/v4";
    if (req.url.rfind(prefix, 0) != 0) return reply(404, "{}");
    const std::string rest = req.url.substr(prefix.size());
    const auto q = rest.find('?');
    const std::string path = rest.substr(0, q);
    const std::string query = q == std::string::npos ? "" : rest.substr(q + 1);
    if (inject) {
      if (auto r = inject(path, query)) return *r;
    }
    const std::string proj = "/projects/acme%2Fmobile-app";
    if (path == "/user") return file("user.synthetic.json");
    if (path == "/projects") return file("projects_membership.synthetic.json", "");
    if (path == proj) return file("project.synthetic.json");
    if (path == proj + "/pipelines") {
      const std::string page = query_param(query, "page");
      if (page.empty() || page == "1") return file("pipelines_page1.synthetic.json", "2");
      if (page == "2") return file("pipelines_page2.synthetic.json", "");
      return reply(200, "[]", "");
    }
    if (path.rfind(proj + "/pipelines/", 0) == 0) {
      const std::string tail = path.substr(proj.size() + 11);
      const auto slash = tail.find('/');
      const std::string pid = tail.substr(0, slash);
      const std::string what = slash == std::string::npos ? "" : tail.substr(slash + 1);
      if (what.empty()) return file("pipeline_" + pid + ".synthetic.json");
      if (what == "jobs") return file("jobs_" + pid + ".synthetic.json", "");
      if (what == "test_report_summary") {
        if (!fixture("test_report_summary_" + pid + ".synthetic.json").empty()) {
          return file("test_report_summary_" + pid + ".synthetic.json");
        }
        return file("test_report_summary_empty.synthetic.json");
      }
    }
    if (path.rfind(proj + "/jobs/", 0) == 0 && path.size() > 6 &&
        path.compare(path.size() - 6, 6, "/trace") == 0) {
      const std::string jid = path.substr(proj.size() + 6, path.size() - proj.size() - 12);
      return file("trace_" + jid + ".synthetic.log");
    }
    if (path == proj + "/deployments") return file("deployments.synthetic.json", "");
    return reply(404, R"({"message":"404 Not Found"})");
  }

  mpi::net::Transport transport() {
    return [this](const mpi::net::HttpRequest& req) { return handle(req); };
  }

  std::vector<std::string> urls_with(const std::string& part) const {
    std::vector<std::string> out;
    for (const auto& r : requests) {
      if (contains(r.url, part)) out.push_back(r.url);
    }
    return out;
  }
};

// ---- a real store in a temporary directory ----

struct TempStore {
  std::string dir;
  std::unique_ptr<sig::SignalStore> store;

  explicit TempStore(sig::ConnectorConfig config = default_config()) {
    std::string tmpl = (fs::temp_directory_path() / "devx-gitlab-XXXXXX").string();
    dir = ::mkdtemp(tmpl.data()) != nullptr ? tmpl : std::string();
    store = std::make_unique<sig::SignalStore>(dir);
    sig::ProjectWorkspace w;
    w.id = "ws";
    w.name = "Acme";
    std::string err;
    MPI_CHECK_MSG(store->save_workspace(w, &err), err);
    MPI_CHECK_MSG(store->save_connector("ws", config, &err), err);
  }
  ~TempStore() {
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
  }

  static sig::ConnectorConfig default_config() {
    sig::ConnectorConfig c;
    c.id = "gitlab-main";
    c.provider = "gitlab";
    c.name = "Acme GitLab";
    c.settings.set("base_url", mpi::json::Value::string(kBase));
    c.settings.set("project", mpi::json::Value::string("acme/mobile-app"));
    return c;
  }

  std::map<std::string, sig::SignalRecord> records() const {
    std::map<std::string, sig::SignalRecord> out;
    for (auto& r : store->all_signals("ws")) out[r.external_id] = r;
    return out;
  }
};

mpi::json::Value run_sync(TempStore& t, FakeGitLab& gl, bool with_token) {
  mpi::intelligence::register_builtin_connectors();
  sig::RunOptions opts;
  opts.transport = gl.transport();
  if (with_token) opts.secret_override = mpi::net::Secret{kToken, "environment"};
  return sig::sync_connector(*t.store, "ws", "gitlab-main", opts);
}

std::string status_of(const mpi::json::Value& v) {
  const auto* s = v.find("status");
  return s != nullptr ? s->as_string() : std::string();
}

std::string attr_str(const sig::SignalRecord& r, const char* key) {
  const auto* v = r.attributes.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string("<absent>");
}

std::int64_t attr_int(const sig::SignalRecord& r, const char* key) {
  const auto* v = r.attributes.find(key);
  return v != nullptr && v->is_number() ? v->as_int() : -1;
}

sig::ConnectorContext direct_context(FakeGitLab& gl) {
  sig::ConnectorContext ctx;
  ctx.transport = gl.transport();
  ctx.workspace_id = "ws";
  ctx.now_iso = "2026-10-08T00:00:00Z";
  return ctx;
}

}  // namespace

// ============================== ci_log ==============================

MPI_TEST(ci_log_gradle_failure_roots_at_what_went_wrong, {}) {
  const FailureSummary f = find_failure(fixture("gradle_failure.synthetic.log"));
  MPI_CHECK(f.found);
  MPI_CHECK_EQ(f.root_error, std::string("Execution failed for task ':app:checkDebugAarMetadata'."));
  MPI_CHECK_EQ(f.root_rule, std::string("gradle_what_went_wrong"));
  const std::string cleaned = mpi::intelligence::clean_ci_log(fixture("gradle_failure.synthetic.log"));
  MPI_CHECK_EQ(mpi::intelligence::excerpt_lines(cleaned, f.root_line - 1, f.root_line - 1, 100),
               std::string("* What went wrong:"));
  // The summary lines and the runner's line are candidates, not the root.
  bool saw_runner = false, saw_task = false;
  for (const auto& c : f.candidates) {
    saw_runner = saw_runner || c.rule == "runner_job_failed";
    saw_task = saw_task || c.text == "> Task :app:checkDebugAarMetadata FAILED";
  }
  MPI_CHECK(saw_runner);
  MPI_CHECK(saw_task);
  MPI_CHECK(f.excerpt_start <= f.root_line && f.root_line <= f.excerpt_end);
  MPI_CHECK(contains(f.excerpt, "Could not find com.acme.analytics:tracker:2.3.1."));
  MPI_CHECK_MSG(!contains(f.excerpt, "\x1b") && !contains(f.excerpt, "section_"),
                "the excerpt is the cleaned log");
}

MPI_TEST(ci_log_npm_failure_skips_npm_bookkeeping, {}) {
  const FailureSummary f = find_failure(fixture("npm_failure.synthetic.log"));
  MPI_CHECK(f.found);
  MPI_CHECK_EQ(f.root_error, std::string("npm ERR! ERESOLVE could not resolve"));
  MPI_CHECK_EQ(f.root_rule, std::string("npm_error"));
  // npm 10's spelling, through the fixture the connector test uses.
  const FailureSummary g = find_failure(fixture("trace_5012.synthetic.log"));
  MPI_CHECK_EQ(g.root_error, std::string("npm error Lifecycle script `lint` failed with error:"));
}

MPI_TEST(ci_log_compiler_error_beats_gradle_and_runner_summaries, {}) {
  const FailureSummary f = find_failure(fixture("compile_error.synthetic.log"));
  MPI_CHECK(f.found);
  MPI_CHECK_EQ(f.root_rule, std::string("compiler_error"));
  MPI_CHECK_MSG(contains(f.root_error, "CartTotals.kt:42:17 Unresolved reference 'discountRate'"),
                f.root_error);  // the earliest of the two compiler errors
  int runner = 0;
  for (const auto& c : f.candidates) {
    if (c.rule == "runner_job_failed") {
      runner++;
      MPI_CHECK_EQ(c.priority, 10);
    }
  }
  MPI_CHECK_EQ(runner, 1);

  // Clang-style diagnostics, and the runner line alone.
  const FailureSummary c = find_failure(
      "$ make\ncc -c src/cart.c\nsrc/cart.c:12:5: error: use of undeclared identifier 'rate'\n"
      "make: *** [cart.o] Error 1\nERROR: Job failed: exit code 2\n");
  MPI_CHECK_EQ(c.root_error, std::string("src/cart.c:12:5: error: use of undeclared identifier 'rate'"));
  MPI_CHECK_EQ(c.root_line, static_cast<std::size_t>(3));
  const FailureSummary r = find_failure("$ ./deploy.sh\nexit 3\nERROR: Job failed: exit code 3\n");
  MPI_CHECK(r.found);
  MPI_CHECK_EQ(r.root_rule, std::string("runner_job_failed"));
}

MPI_TEST(ci_log_strips_ansi_and_section_markers, {}) {
  const std::string raw =
      "\x1b[0Ksection_start:1759655547:step_script\r\x1b[0K\x1b[0K\x1b[36;1mExecuting step\x1b[0;m\n"
      "\x1b[32;1m$ make\x1b[0;m\n"
      "section_end:1759655708:step_script\r\x1b[0K\n"
      "\x1b]8;;https://example.com\x07link\x1b]8;;\x07 text\n"
      "a section_start: mentioned in prose stays\n"
      "section_start:1:named[collapsed=true]\r\x1b[0Kafter\n";
  const std::string clean = mpi::intelligence::clean_ci_log(raw);
  MPI_CHECK_EQ(clean, std::string("Executing step\n$ make\nlink text\n"
                                  "a section_start: mentioned in prose stays\nafter\n"));
  const std::string fixture_clean = mpi::intelligence::clean_ci_log(fixture("trace_5011.synthetic.log"));
  MPI_CHECK(!contains(fixture_clean, "\x1b"));
  MPI_CHECK(!contains(fixture_clean, "\r"));
  MPI_CHECK(!contains(fixture_clean, "section_start:"));
  MPI_CHECK(!contains(fixture_clean, "section_end:"));
  MPI_CHECK(contains(fixture_clean, "\nERROR: Job failed: exit code 1\n"));
  // Cleaning is idempotent, so line numbers from either side agree.
  MPI_CHECK_EQ(mpi::intelligence::clean_ci_log(fixture_clean), fixture_clean);
}

MPI_TEST(ci_log_carriage_return_keeps_what_the_terminal_showed, {}) {
  const std::string clean = mpi::intelligence::clean_ci_log(
      "Downloading 10%\rDownloading 55%\rDownloading 100%\n"
      "windows line\r\n"
      "done\x1b[0m\r\x1b[0K\n"
      "\r\n");
  MPI_CHECK_EQ(clean, std::string("Downloading 100%\nwindows line\ndone\n\n"));
  const std::string gradle = mpi::intelligence::clean_ci_log(fixture("trace_5011.synthetic.log"));
  MPI_CHECK(contains(gradle, "\n.............100%\n"));
  MPI_CHECK(!contains(gradle, "10%."));
}

MPI_TEST(ci_log_twenty_megabyte_log_is_bounded_and_fast, {}) {
  // 200k lines, ~20 MB: colour codes on every tenth line, a CR progress line
  // every thousandth, a few minified-bundle lines, and the failure near the
  // end surrounded by 60 KB lines that must not blow the excerpt.
  std::string log;
  log.reserve(21u * 1024u * 1024u);
  const std::string filler(80, 'x');
  for (int i = 0; i < 200000; i++) {
    if (i == 199950) {
      log += "src/app/Checkout.kt:99:1: error: type mismatch: inferred type is String but Int was expected\n";
    }
    if (i > 199930 && i < 199970 && i % 5 == 0) {
      log += "bundle.min.js " + std::string(60000, 'm') + "\n";
      continue;
    }
    if (i % 1000 == 0) {
      log += "progress 10%\rprogress 50%\rprogress 100% " + filler + "\n";
    } else if (i % 10 == 0) {
      log += "\x1b[32;1m[" + std::to_string(i) + "] compiling module " + filler + "\x1b[0;m\n";
    } else {
      log += "[" + std::to_string(i) + "] compiling module " + filler + "\n";
    }
  }
  log += "ERROR: Job failed: exit code 1\n";
  MPI_CHECK(log.size() > 18u * 1024u * 1024u);
  const auto t0 = std::chrono::steady_clock::now();
  const FailureSummary f = find_failure(log);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  MPI_CHECK_MSG(secs < 2.0, "took " + std::to_string(secs) + " s");
  MPI_CHECK(f.found);
  MPI_CHECK_EQ(f.root_rule, std::string("compiler_error"));
  MPI_CHECK(f.total_lines > 200000u);
  MPI_CHECK(f.excerpt.size() <= 8192u);
  MPI_CHECK(contains(f.excerpt, "type mismatch"));
  MPI_CHECK(f.excerpt_start <= f.root_line && f.root_line <= f.excerpt_end);
  MPI_CHECK(contains(f.excerpt, "bytes cut]"));
  MPI_CHECK(f.candidates.size() <= 20u);
  // The excerpt range names exactly the lines it holds.
  const std::string again = mpi::intelligence::excerpt_lines(log, f.excerpt_start, f.excerpt_end, 1u << 22);
  MPI_CHECK_EQ(again.substr(0, 40), f.excerpt.substr(0, 40));
}

MPI_TEST(ci_log_without_a_failure_reports_none, {}) {
  const FailureSummary f = find_failure(fixture("passing.synthetic.log"));
  MPI_CHECK_MSG(!f.found, f.root_error);
  MPI_CHECK(f.root_error.empty());
  MPI_CHECK(f.candidates.empty());
  MPI_CHECK(f.excerpt.empty());
  MPI_CHECK(f.total_lines > 5u);
  MPI_CHECK(!find_failure("").found);
  const auto j = f.to_json();
  MPI_CHECK(!field(j, "found").as_bool(true));
  MPI_CHECK(j.find("root_error") == nullptr);
}

MPI_TEST(ci_log_tests_traceback_and_candidate_cap, {}) {
  // Gradle unit-test failure: the failing test outranks Gradle's summary.
  const FailureSummary g = find_failure(fixture("trace_5011.synthetic.log"));
  MPI_CHECK_EQ(g.root_error, std::string("com.acme.cart.CartTotalsTest > appliesPercentageDiscount FAILED"));
  MPI_CHECK_EQ(g.root_rule, std::string("test_failure"));

  const FailureSummary py = find_failure(
      "$ pytest\nTraceback (most recent call last):\n  File \"app.py\", line 3, in <module>\n"
      "    main()\nValueError: price must be positive\nERROR: Job failed: exit code 1\n");
  MPI_CHECK_EQ(py.root_error, std::string("ValueError: price must be positive"));
  MPI_CHECK_EQ(py.root_rule, std::string("python_traceback"));

  // 50 equal-priority lines and one stronger late one: 20 kept, root kept.
  std::string many;
  for (int i = 0; i < 50; i++) many += "fatal: attempt " + std::to_string(i) + "\n";
  many += "error: the real one\n";
  const FailureSummary m = find_failure(many);
  MPI_CHECK_EQ(m.candidates.size(), static_cast<std::size_t>(20));
  MPI_CHECK_EQ(m.root_error, std::string("error: the real one"));
  MPI_CHECK_EQ(m.candidates.front().text, std::string("fatal: attempt 0"));
  MPI_CHECK_EQ(m.candidates.back().text, std::string("error: the real one"));

  const FailureSummary longline = find_failure("error: " + std::string(1000, 'z') + "\n");
  MPI_CHECK_EQ(longline.root_error.size(), static_cast<std::size_t>(300));
}

// ============================== connector ==============================

MPI_TEST(gitlab_info_and_capabilities, {}) {
  auto c = mpi::intelligence::make_gitlab_connector();
  const auto i = c->info();
  MPI_CHECK_EQ(i.provider, std::string("gitlab"));
  MPI_CHECK_EQ(i.category, std::string("ci_cd"));
  MPI_CHECK_EQ(i.credential_env, std::string("GITLAB_TOKEN"));
  MPI_CHECK_EQ(i.credential_service, std::string("com.devx.gitlab"));
  MPI_CHECK(i.credential_optional);
  MPI_CHECK(contains(i.credential_help, "optional for public projects"));
  const auto caps = c->capabilities();
  MPI_CHECK(caps.ci_pipelines && caps.ci_jobs && caps.deployments && caps.tests && caps.incremental_pull);
  MPI_CHECK_MSG(!caps.artifacts, "binaries are never downloaded");
  for (const auto& [name, _] : i.settings) MPI_CHECK_MSG(!sig::key_looks_secret(name), name);
}

MPI_TEST(gitlab_validate_anonymous_and_with_token, {}) {
  FakeGitLab gl;
  auto c = mpi::intelligence::make_gitlab_connector();
  auto ctx = direct_context(gl);
  const auto anon = c->validate(TempStore::default_config(), ctx);
  MPI_CHECK_MSG(anon.ok, anon.error);
  MPI_CHECK(anon.account.empty());
  bool visibility = false;
  for (const auto& n : anon.notes) visibility = visibility || contains(n, "visibility public");
  MPI_CHECK(visibility);
  MPI_CHECK(gl.urls_with("/user").empty());
  for (const auto& r : gl.requests) {
    MPI_CHECK(r.headers.empty());
    MPI_CHECK_EQ(r.allowed_hosts.size(), static_cast<std::size_t>(1));
    MPI_CHECK_EQ(r.allowed_hosts[0], std::string("gitlab.example.com"));
  }

  TempStore t;
  FakeGitLab gl2;
  mpi::intelligence::register_builtin_connectors();
  sig::RunOptions opts;
  opts.transport = gl2.transport();
  opts.secret_override = mpi::net::Secret{kToken, "environment"};
  const auto v = sig::validate_connector(*t.store, "ws", "gitlab-main", opts);
  MPI_CHECK_MSG(field(v, "ok").as_bool(), v.dump());
  MPI_CHECK_EQ(field(v, "account").as_string(), std::string("dev.example"));
  MPI_CHECK_EQ(gl2.urls_with("/user").size(), static_cast<std::size_t>(1));
  for (const auto& r : gl2.requests) {
    MPI_CHECK_EQ(r.headers.size(), static_cast<std::size_t>(1));
    MPI_CHECK_EQ(r.headers[0].first, std::string("PRIVATE-TOKEN"));
    MPI_CHECK_EQ(r.headers[0].second, std::string(kToken));
  }

  // A private project without a token: a clear error, not a guess.
  FakeGitLab gl3;
  gl3.inject = [](const std::string&, const std::string&) -> std::optional<mpi::net::HttpResponse> {
    return FakeGitLab::reply(404, R"({"message":"404 Project Not Found"})");
  };
  auto ctx3 = direct_context(gl3);
  const auto missing = c->validate(TempStore::default_config(), ctx3);
  MPI_CHECK(!missing.ok);
  MPI_CHECK_MSG(contains(missing.error, "404") && contains(missing.error, "private project"), missing.error);
}

MPI_TEST(gitlab_discover_needs_a_token, {}) {
  FakeGitLab gl;
  auto c = mpi::intelligence::make_gitlab_connector();
  auto ctx = direct_context(gl);
  const auto anon = c->discover(TempStore::default_config(), ctx);
  MPI_CHECK(!anon.ok);
  MPI_CHECK_MSG(contains(anon.error, "token") && contains(anon.error, "group/name"), anon.error);
  MPI_CHECK(gl.requests.empty());

  ctx.secret = mpi::net::Secret{kToken, "environment"};
  const auto d = c->discover(TempStore::default_config(), ctx);
  MPI_CHECK_MSG(d.ok, d.error);
  REQUIRE(d.resources.size() == 2u);
  MPI_CHECK_EQ(field(d.resources.items()[0], "name").as_string(), std::string("acme/mobile-app"));
  MPI_CHECK_EQ(field(d.resources.items()[0], "id").as_string(), std::string("4242"));
  MPI_CHECK_EQ(field(d.resources.items()[0], "kind").as_string(), std::string("project"));
  MPI_CHECK(contains(gl.requests.back().url, "membership=true") && contains(gl.requests.back().url, "simple=true"));
}

MPI_TEST(gitlab_full_sync_into_the_store, {}) {
  TempStore t;
  FakeGitLab gl;
  const auto res = run_sync(t, gl, true);
  MPI_CHECK_MSG(status_of(res) == "complete", res.dump());
  // 3 pipelines, 6 jobs, 2 test summaries, 2 deployments.
  MPI_CHECK_EQ(field(res, "records_written").as_int(), static_cast<std::int64_t>(13));
  // 3 pipeline replies, 2 failed-job logs, 2 test summaries.
  MPI_CHECK_EQ(field(res, "raw_written").as_int(), static_cast<std::int64_t>(7));
  const auto recs = t.records();
  REQUIRE(recs.size() == 13u);
  for (const char* id : {"pipeline:1000", "pipeline:1001", "pipeline:1002", "job:5000", "job:5010",
                         "job:5011", "job:5012", "job:5020", "job:5021", "tests:1001", "tests:1002",
                         "deploy:301", "deploy:302"}) {
    REQUIRE(recs.count(id) == 1u);
  }

  // Pipelines.
  const auto& p = recs.at("pipeline:1001");
  MPI_CHECK_EQ(p.kind, std::string("pipeline"));
  MPI_CHECK_EQ(p.provider, std::string("gitlab"));
  MPI_CHECK_EQ(p.severity.value_or(""), std::string("error"));
  MPI_CHECK_EQ(p.title.value_or(""), std::string("feature/cart-discounts pipeline #1001: failed"));
  MPI_CHECK_EQ(p.occurred_at, std::string("2026-10-05T09:12:07.731Z"));
  MPI_CHECK_EQ(p.release.commit_sha.value_or(""), std::string("9f2c1e7a5b3d4c6e8f0a1b2c3d4e5f6a7b8c9d0e"));
  MPI_CHECK_EQ(p.release.branch.value_or(""), std::string("feature/cart-discounts"));
  MPI_CHECK(p.release.source == sig::FactSource::kProvider);
  MPI_CHECK(p.basis == sig::EvidenceBasis::kExact);
  MPI_CHECK_EQ(attr_str(p, "status"), std::string("failed"));
  MPI_CHECK_EQ(attr_str(p, "source"), std::string("merge_request_event"));
  MPI_CHECK_EQ(attr_int(p, "duration"), static_cast<std::int64_t>(380));
  MPI_CHECK_EQ(attr_str(p, "provider_url"), std::string("https://gitlab.example.com/acme/mobile-app/-/pipelines/1001"));
  MPI_CHECK_MSG(!contains(p.to_json().dump(), "dev.example") && !contains(p.to_json().dump(), "avatar"),
                "the pipeline's user stays in raw evidence only");
  const auto raw = t.store->read_raw("ws", p.raw_ref, 0, 1u << 20);
  MPI_CHECK_MSG(raw.ok, raw.error);
  MPI_CHECK(contains(raw.bytes, "\"username\": \"dev.example\""));
  MPI_CHECK_EQ(recs.at("pipeline:1002").severity.value_or(""), std::string("info"));
  const auto& tagp = recs.at("pipeline:1000");
  MPI_CHECK_EQ(tagp.severity.value_or(""), std::string("warning"));
  MPI_CHECK_MSG(!tagp.release.branch.has_value(), "a tag is not a branch");
  MPI_CHECK(field(tagp.attributes, "tag").as_bool());
  MPI_CHECK_EQ(attr_str(tagp, "ref"), std::string("v2.8.0"));

  // Jobs: the failed one has its log stored and its root error found.
  const auto& j = recs.at("job:5011");
  MPI_CHECK_EQ(j.kind, std::string("ci_job"));
  MPI_CHECK_EQ(j.severity.value_or(""), std::string("error"));
  MPI_CHECK_EQ(j.title.value_or(""), std::string("test/unitTest: failed"));
  MPI_CHECK(j.basis == sig::EvidenceBasis::kExact);
  MPI_CHECK_EQ(attr_str(j, "failure_reason"), std::string("script_failure"));
  MPI_CHECK_EQ(attr_str(j, "root_error"), std::string("com.acme.cart.CartTotalsTest > appliesPercentageDiscount FAILED"));
  MPI_CHECK(attr_int(j, "root_line") > 0);
  MPI_CHECK(attr_int(j, "excerpt_start") <= attr_int(j, "root_line"));
  MPI_CHECK(attr_int(j, "failure_candidates") > 1);
  MPI_CHECK_MSG(j.attributes.find("excerpt") == nullptr, "excerpts are cut from raw on demand");
  MPI_CHECK(j.attributes.find("log_truncated") == nullptr);
  MPI_CHECK_EQ(j.raw_ref, std::string("raw/gitlab/jobs/5011.log"));
  const auto log = t.store->read_raw("ws", j.raw_ref, 0, 1u << 20);
  MPI_CHECK(log.ok && log.bytes == fixture("trace_5011.synthetic.log"));
  const std::string cut = mpi::intelligence::excerpt_lines(log.bytes, static_cast<std::size_t>(attr_int(j, "root_line")),
                                                           static_cast<std::size_t>(attr_int(j, "root_line")), 4096);
  MPI_CHECK_EQ(cut, attr_str(j, "root_error"));
  const auto& lint = recs.at("job:5012");
  MPI_CHECK_EQ(lint.severity.value_or(""), std::string("warning"));  // allow_failure
  MPI_CHECK(field(lint.attributes, "allow_failure").as_bool());
  MPI_CHECK(contains(attr_str(lint, "root_error"), "Lifecycle script `lint` failed"));
  const auto& ok_job = recs.at("job:5010");
  MPI_CHECK_EQ(ok_job.severity.value_or(""), std::string("info"));
  MPI_CHECK(ok_job.raw_ref.empty());
  MPI_CHECK_EQ(gl.urls_with("/trace").size(), static_cast<std::size_t>(2));  // failed jobs only
  MPI_CHECK_EQ(recs.at("job:5000").severity.value_or(""), std::string("warning"));

  // Test report summaries: only where there were tests.
  const auto& tr = recs.at("tests:1001");
  MPI_CHECK_EQ(tr.kind, std::string("test"));
  MPI_CHECK_EQ(tr.severity.value_or(""), std::string("error"));
  MPI_CHECK_EQ(attr_int(tr, "total"), static_cast<std::int64_t>(214));
  MPI_CHECK_EQ(attr_int(tr, "failed"), static_cast<std::int64_t>(2));
  MPI_CHECK_EQ(attr_int(tr, "skipped"), static_cast<std::int64_t>(1));
  const auto* suites = tr.attributes.find("suites");
  REQUIRE(suites != nullptr && suites->size() == 2u);
  MPI_CHECK_EQ(field(suites->items()[0], "name").as_string(), std::string("unitTest"));  // failures first
  MPI_CHECK_EQ(recs.at("tests:1002").severity.value_or(""), std::string("info"));
  MPI_CHECK(recs.find("tests:1000") == recs.end());

  // Deployments.
  const auto& d = recs.at("deploy:301");
  MPI_CHECK_EQ(d.kind, std::string("deploy"));
  MPI_CHECK_EQ(d.severity.value_or(""), std::string("error"));
  MPI_CHECK_EQ(d.environment.value_or(""), std::string("review-cart-discounts"));
  MPI_CHECK_EQ(d.release.environment.value_or(""), std::string("review-cart-discounts"));
  MPI_CHECK_EQ(d.release.commit_sha.value_or(""), std::string("9f2c1e7a5b3d4c6e8f0a1b2c3d4e5f6a7b8c9d0e"));
  MPI_CHECK(d.basis == sig::EvidenceBasis::kExact);
  MPI_CHECK_EQ(d.occurred_at, std::string("2026-10-05T09:19:58.900Z"));
  MPI_CHECK_EQ(attr_int(d, "deployable_job_id"), static_cast<std::int64_t>(5013));
  MPI_CHECK(!contains(d.to_json().dump(), "dev.example"));
  MPI_CHECK_EQ(recs.at("deploy:302").severity.value_or(""), std::string("info"));

  // Pagination: X-Next-Page 2, then empty -- exactly two pipeline pages.
  const auto lists = gl.urls_with("/pipelines?");
  REQUIRE(lists.size() == 2u);
  MPI_CHECK(contains(lists[0], "page=1") && contains(lists[1], "page=2"));
  MPI_CHECK(contains(lists[0], "order_by=updated_at") && contains(lists[0], "sort=desc"));
  MPI_CHECK(contains(lists[0], "per_page=50") && contains(lists[0], "updated_after="));

  // The watermark is the newest updated_at GitLab reported.
  const auto cur = t.store->cursor("ws", "gitlab-main");
  MPI_CHECK_EQ(cur.watermark, std::string("2026-10-06T11:02:10.481Z"));
  MPI_CHECK(cur.resume.empty());
}

MPI_TEST(gitlab_rate_limit_is_partial_then_resumes, {}) {
  TempStore t;
  FakeGitLab gl;
  gl.inject = [](const std::string& path, const std::string& query) -> std::optional<mpi::net::HttpResponse> {
    if (path == "/projects/acme%2Fmobile-app/pipelines" && query_param(query, "page") == "2") {
      auto r = FakeGitLab::reply(429, R"({"message":"429 Too Many Requests"})");
      r.headers["retry-after"] = "30";
      return r;
    }
    return std::nullopt;
  };
  const auto first = run_sync(t, gl, true);
  MPI_CHECK_MSG(status_of(first) == "partial", first.dump());
  MPI_CHECK(field(first, "rate_limited").as_bool());
  MPI_CHECK_EQ(field(first, "retry_after_s").as_int(), static_cast<std::int64_t>(30));
  MPI_CHECK(t.records().count("pipeline:1001") == 1u);  // page 1 kept
  MPI_CHECK(t.records().count("pipeline:1000") == 0u);
  MPI_CHECK(t.records().count("deploy:301") == 0u);
  auto cur = t.store->cursor("ws", "gitlab-main");
  MPI_CHECK_EQ(cur.resume, std::string("2"));
  MPI_CHECK_MSG(cur.watermark.empty(), "a partial sync does not advance the watermark");
  const auto first_lists = gl.urls_with("/pipelines?");
  REQUIRE(!first_lists.empty());
  const std::string first_since =
      query_param(first_lists[0].substr(first_lists[0].find('?') + 1), "updated_after");
  MPI_CHECK(!first_since.empty());

  FakeGitLab again;
  const auto second = run_sync(t, again, true);
  MPI_CHECK_MSG(status_of(second) == "complete", second.dump());
  const auto lists = again.urls_with("/pipelines?");
  REQUIRE(lists.size() == 1u);
  MPI_CHECK(contains(lists[0], "page=2"));
  MPI_CHECK_MSG(contains(lists[0], "updated_after=" + first_since), "resumed in the same window");
  MPI_CHECK(t.records().count("pipeline:1000") == 1u);
  MPI_CHECK(t.records().count("deploy:301") == 1u);
  cur = t.store->cursor("ws", "gitlab-main");
  MPI_CHECK(cur.resume.empty());
  MPI_CHECK(!cur.watermark.empty());

  // A failure before any page is a failed sync, and leaves the cursor alone.
  FakeGitLab down;
  down.inject = [](const std::string&, const std::string&) -> std::optional<mpi::net::HttpResponse> {
    mpi::net::HttpResponse r;
    r.error = "could not resolve host";
    return r;
  };
  const auto failed = run_sync(t, down, true);
  MPI_CHECK_EQ(status_of(failed), std::string("failed"));
  MPI_CHECK(contains(field(failed, "error").as_string(), "could not reach GitLab"));
  MPI_CHECK_EQ(t.store->cursor("ws", "gitlab-main").watermark, cur.watermark);
}

MPI_TEST(gitlab_incremental_sync_asks_after_the_watermark, {}) {
  TempStore t;
  FakeGitLab gl;
  MPI_CHECK_EQ(status_of(run_sync(t, gl, true)), std::string("complete"));
  const std::string wm = t.store->cursor("ws", "gitlab-main").watermark;
  MPI_CHECK(!wm.empty());
  FakeGitLab next;
  MPI_CHECK_EQ(status_of(run_sync(t, next, true)), std::string("complete"));
  const std::string enc = mpi::net::url_encode(wm);
  const auto lists = next.urls_with("/pipelines?");
  REQUIRE(!lists.empty());
  MPI_CHECK_MSG(contains(lists[0], "updated_after=" + enc + "&"), lists[0]);
  const auto deps = next.urls_with("/deployments?");
  MPI_CHECK(!deps.empty() && contains(deps[0], "updated_after=" + enc + "&"));
  // Re-reading the same objects updates records in place, never duplicates.
  MPI_CHECK_EQ(t.records().size(), static_cast<std::size_t>(13));
}

MPI_TEST(gitlab_token_travels_only_in_the_header, {}) {
  TempStore t;
  FakeGitLab gl;
  const auto res = run_sync(t, gl, true);
  MPI_CHECK_EQ(status_of(res), std::string("complete"));
  MPI_CHECK(!contains(res.dump(), kToken));
  MPI_CHECK(!gl.requests.empty());
  for (const auto& r : gl.requests) {
    MPI_CHECK_MSG(!contains(r.url, kToken), r.url);
    MPI_CHECK(!contains(r.body, kToken));
    MPI_CHECK_EQ(mpi::net::url_host(r.url), std::string("gitlab.example.com"));
    MPI_CHECK(r.allowed_hosts == std::vector<std::string>{"gitlab.example.com"});
    int carrying = 0;
    for (const auto& [k, v] : r.headers) {
      if (contains(v, kToken)) {
        carrying++;
        MPI_CHECK_EQ(k, std::string("PRIVATE-TOKEN"));
      }
    }
    MPI_CHECK_EQ(carrying, 1);
  }
  int files = 0;
  for (const auto& e : fs::recursive_directory_iterator(t.dir)) {
    if (!e.is_regular_file()) continue;
    files++;
    MPI_CHECK_MSG(!contains(read_file(e.path().string()), kToken), e.path().string());
  }
  MPI_CHECK(files > 10);
}

MPI_TEST(gitlab_anonymous_sync_needs_no_secret, {}) {
  // Called directly rather than through sync_connector, which would look up
  // GITLAB_TOKEN / the Keychain of whoever runs the test.
  TempStore t;
  FakeGitLab gl;
  auto c = mpi::intelligence::make_gitlab_connector();
  auto ctx = direct_context(gl);
  MPI_CHECK(!ctx.secret.has_value());
  sig::SyncResult r;
  {
    auto sink = t.store->open_sink("ws", "gitlab-main");
    r = c->sync(TempStore::default_config(), sig::SyncCursor{}, *sink, ctx);
  }
  MPI_CHECK_MSG(r.status == sig::SyncStatus::kComplete, r.error);
  MPI_CHECK_EQ(r.records_written, 13);
  for (const auto& req : gl.requests) MPI_CHECK(req.headers.empty());
  // The first sync's window is lookback_days before now.
  REQUIRE(!gl.requests.empty() && t.records().count("job:5011") == 1u);
  MPI_CHECK(contains(gl.requests[0].url, "updated_after=2026-09-24T00%3A00%3A00Z"));
  MPI_CHECK_EQ(field(t.records().at("job:5011").attributes, "root_error").as_string(),
               std::string("com.acme.cart.CartTotalsTest > appliesPercentageDiscount FAILED"));
}

MPI_TEST(gitlab_settings_bounds_and_errors, {}) {
  FakeGitLab gl;
  auto c = mpi::intelligence::make_gitlab_connector();
  auto ctx = direct_context(gl);
  TempStore t;

  sig::ConnectorConfig plain = TempStore::default_config();
  plain.settings.set("base_url", mpi::json::Value::string("http://gitlab.example.com"));
  MPI_CHECK(contains(c->validate(plain, ctx).error, "https"));

  sig::ConnectorConfig none = TempStore::default_config();
  none.settings = mpi::json::Value::object();
  {
    auto sink = t.store->open_sink("ws", "gitlab-main");
    const auto r = c->sync(none, sig::SyncCursor{}, *sink, ctx);
    MPI_CHECK(r.status == sig::SyncStatus::kFailed);
    MPI_CHECK(contains(r.error, "project"));
  }
  MPI_CHECK(gl.requests.empty());

  // Numeric project id goes in the URL as is; max_pipelines and logs bound.
  sig::ConnectorConfig small = TempStore::default_config();
  small.settings.set("project", mpi::json::Value::string("4242"));
  small.settings.set("max_pipelines", mpi::json::Value::integer(1));
  small.settings.set("max_job_logs", mpi::json::Value::integer(0));
  small.settings.set("ref", mpi::json::Value::string("main"));
  FakeGitLab gl2;
  gl2.inject = [](const std::string& path, const std::string&) -> std::optional<mpi::net::HttpResponse> {
    // Serve the numeric id from the same fixtures.
    if (path.rfind("/projects/4242", 0) != 0) return std::nullopt;
    FakeGitLab inner;
    mpi::net::HttpRequest req;
    req.url = std::string(kBase) + "/api/v4/projects/acme%2Fmobile-app" + path.substr(14);
    return inner.handle(req);
  };
  auto ctx2 = direct_context(gl2);
  sig::SyncResult r;
  {
    auto sink = t.store->open_sink("ws", "gitlab-main");
    r = c->sync(small, sig::SyncCursor{}, *sink, ctx2);
  }
  MPI_CHECK_MSG(r.status == sig::SyncStatus::kPartial, r.error);
  MPI_CHECK_MSG(r.cursor.resume == "1", r.cursor.resume);  // page 1 still has a pipeline unread
  MPI_CHECK(gl2.urls_with("/trace").empty());
  REQUIRE(!gl2.requests.empty());
  MPI_CHECK(contains(gl2.requests[0].url, "/projects/4242/pipelines?"));
  MPI_CHECK(contains(gl2.requests[0].url, "&ref=main"));
  bool noted = false;
  for (const auto& n : r.notes) noted = noted || contains(n, "max_pipelines");
  MPI_CHECK(noted);
}

MPI_TEST(gitlab_truncated_log_and_cancel_are_said, {}) {
  TempStore t;
  FakeGitLab gl;
  gl.inject = [](const std::string& path, const std::string&) -> std::optional<mpi::net::HttpResponse> {
    if (path != "/projects/acme%2Fmobile-app/jobs/5011/trace") return std::nullopt;
    auto r = FakeGitLab::reply(200, fixture("trace_5011.synthetic.log").substr(0, 2048));
    r.truncated = true;
    return r;
  };
  sig::ConnectorConfig cfg = TempStore::default_config();
  cfg.settings.set("max_log_bytes", mpi::json::Value::integer(2048));
  auto c = mpi::intelligence::make_gitlab_connector();
  auto ctx = direct_context(gl);
  sig::SyncResult r;
  {
    auto sink = t.store->open_sink("ws", "gitlab-main");
    r = c->sync(cfg, sig::SyncCursor{}, *sink, ctx);
  }
  MPI_CHECK_MSG(r.status == sig::SyncStatus::kComplete, r.error);
  REQUIRE(t.records().count("job:5011") == 1u);
  const auto j = t.records().at("job:5011");
  MPI_CHECK(field(j.attributes, "log_truncated").as_bool());
  bool noted = false;
  for (const auto& n : r.notes) noted = noted || (contains(n, "job 5011") && contains(n, "2048"));
  MPI_CHECK(noted);
  for (const auto& req : gl.requests) {
    if (contains(req.url, "/jobs/5011/trace")) MPI_CHECK_EQ(req.max_body_bytes, static_cast<std::size_t>(2048));
  }

  // Cancelled before anything: a failed sync that says why, and no request.
  mpi::CancellationSource cancel;
  cancel.cancel();
  FakeGitLab quiet;
  auto ctx2 = direct_context(quiet);
  ctx2.cancel = cancel.token();
  {
    auto sink = t.store->open_sink("ws", "gitlab-main");
    r = c->sync(cfg, sig::SyncCursor{}, *sink, ctx2);
  }
  MPI_CHECK(r.status == sig::SyncStatus::kFailed);
  MPI_CHECK_EQ(r.error, std::string("cancelled"));
  MPI_CHECK(quiet.requests.empty());
}

MPI_TEST(gitlab_merge_request_ref_is_not_a_branch, {}) {
  // As gitlab.com serves a merge-request pipeline: the ref is a hidden ref,
  // and the SHA is still exactly what was built.
  TempStore t;
  FakeGitLab gl;
  gl.inject = [](const std::string& path, const std::string&) -> std::optional<mpi::net::HttpResponse> {
    if (path != "/projects/acme%2Fmobile-app/pipelines/1001") return std::nullopt;
    std::string body = fixture("pipeline_1001.synthetic.json");
    const std::string from = "\"ref\": \"feature/cart-discounts\"";
    const auto at = body.find(from);
    if (at == std::string::npos) return std::nullopt;
    body.replace(at, from.size(), "\"ref\": \"refs/merge-requests/57/merge\"");
    return FakeGitLab::reply(200, body);
  };
  MPI_CHECK_EQ(status_of(run_sync(t, gl, true)), std::string("complete"));
  const auto recs = t.records();
  REQUIRE(recs.count("pipeline:1001") == 1u);
  const auto& p = recs.at("pipeline:1001");
  MPI_CHECK(!p.release.branch.has_value());
  MPI_CHECK(p.basis == sig::EvidenceBasis::kExact);
  MPI_CHECK_EQ(attr_str(p, "ref"), std::string("refs/merge-requests/57/merge"));
  MPI_CHECK_EQ(p.title.value_or(""), std::string("refs/merge-requests/57/merge pipeline #1001: failed"));
}
