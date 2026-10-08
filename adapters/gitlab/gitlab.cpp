#include "adapters/gitlab/gitlab.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "adapters/gitlab/ci_log.hpp"
#include "core/util/time.hpp"

namespace mpi::intelligence {
namespace {

namespace sig = mpi::signals;

constexpr const char* kProvider = "gitlab";
constexpr int kPipelinesPerPage = 50;
constexpr int kJobsPerPage = 100;
constexpr int kDeploymentsPerPage = 50;
constexpr int kProjectsPerPage = 50;
constexpr std::size_t kMiB = 1024ull * 1024ull;
constexpr std::size_t kDefaultMaxLogBytes = 20 * kMiB;
// The store refuses raw files past 64 MiB; a log setting above that could
// never be kept.
constexpr std::size_t kMaxLogBytesCeiling = 64 * kMiB;
constexpr std::size_t kMaxSuites = 20;
constexpr std::size_t kMaxMessageBytes = 200;

// ---- small JSON helpers ----

std::optional<std::string> str_of(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (v == nullptr || !v->is_string() || v->as_string().empty()) return std::nullopt;
  return v->as_string();
}

std::optional<std::int64_t> int_of(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (v == nullptr || !v->is_number()) return std::nullopt;
  return v->as_int();
}

bool bool_of(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v != nullptr && v->is_bool() && v->as_bool();
}

const json::Value* obj_of(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v != nullptr && v->is_object() ? v : nullptr;
}

// Copies a scalar the provider reported. null and absent stay absent: "no
// duration" is not a duration of zero.
void copy_scalar(json::Value& dst, const char* dst_key, const json::Value& src, const char* src_key) {
  const json::Value* v = src.find(src_key);
  if (v == nullptr || v->is_null() || v->is_array() || v->is_object()) return;
  if (v->is_string() && v->as_string().empty()) return;
  dst.set(dst_key, *v);
}

void set_str(json::Value& o, const char* key, const std::optional<std::string>& v) {
  if (v) o.set(key, json::Value::string(*v));
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool all_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// ---- settings ----

struct Settings {
  std::string base;        // https://host[/prefix], no trailing slash
  std::string host;        // the only host requests may reach
  std::string project;     // as configured
  std::string project_id;  // as it goes in a URL: digits, or the encoded path
  std::string ref;
  int lookback_days = 14;
  int max_pipelines = 50;
  bool fetch_logs = true;
  std::size_t max_log_bytes = kDefaultMaxLogBytes;
  int max_job_logs = 20;
};

std::optional<std::string> setting_text(const json::Value& s, const char* key) {
  const json::Value* v = s.find(key);
  if (v == nullptr) return std::nullopt;
  if (v->is_int()) return std::to_string(v->as_int());
  if (!v->is_string()) return std::nullopt;
  std::string t = trim(v->as_string());
  if (t.empty()) return std::nullopt;
  return t;
}

// A whole number from a number or a string of digits; `error` when the
// setting is there but is not one, so a typo is reported, not ignored.
std::optional<std::int64_t> setting_number(const json::Value& s, const char* key, std::string* error) {
  const json::Value* v = s.find(key);
  if (v == nullptr || v->is_null()) return std::nullopt;
  if (v->is_number()) return v->as_int();
  if (v->is_string() && all_digits(trim(v->as_string()))) {
    return static_cast<std::int64_t>(std::stoll(trim(v->as_string())));
  }
  *error = std::string("setting '") + key + "' must be a whole number";
  return std::nullopt;
}

std::optional<bool> setting_bool(const json::Value& s, const char* key, std::string* error) {
  const json::Value* v = s.find(key);
  if (v == nullptr || v->is_null()) return std::nullopt;
  if (v->is_bool()) return v->as_bool();
  if (v->is_string()) {
    const std::string t = lower(trim(v->as_string()));
    if (t == "true" || t == "1" || t == "yes") return true;
    if (t == "false" || t == "0" || t == "no") return false;
  }
  *error = std::string("setting '") + key + "' must be true or false";
  return std::nullopt;
}

std::optional<Settings> read_settings(const json::Value& raw, std::string* error) {
  Settings s;
  std::string base = setting_text(raw, "base_url").value_or("https://gitlab.com");
  while (!base.empty() && base.back() == '/') base.pop_back();
  s.base = base;
  s.host = net::url_host(base);
  if (s.host.empty()) {
    *error = "base_url must be an https URL such as https://gitlab.com (got '" + base + "')";
    return std::nullopt;
  }
  if (auto p = setting_text(raw, "project")) {
    std::string project = *p;
    // A pasted project URL is the path it names.
    if (project.rfind(s.base + "/", 0) == 0) project = project.substr(s.base.size() + 1);
    while (!project.empty() && project.front() == '/') project.erase(0, 1);
    while (!project.empty() && project.back() == '/') project.pop_back();
    if (project.size() > 4 && project.compare(project.size() - 4, 4, ".git") == 0) {
      project.resize(project.size() - 4);
    }
    s.project = project;
    s.project_id = all_digits(project) ? project : net::url_encode(project);
  }
  s.ref = setting_text(raw, "ref").value_or("");
  std::string err;
  if (auto v = setting_number(raw, "lookback_days", &err)) {
    s.lookback_days = static_cast<int>(std::clamp<std::int64_t>(*v, 1, 3650));
  }
  if (auto v = setting_number(raw, "max_pipelines", &err)) {
    s.max_pipelines = static_cast<int>(std::clamp<std::int64_t>(*v, 1, 10000));
  }
  if (auto v = setting_bool(raw, "fetch_failed_job_logs", &err)) s.fetch_logs = *v;
  if (auto v = setting_number(raw, "max_log_bytes", &err)) {
    s.max_log_bytes = static_cast<std::size_t>(
        std::clamp<std::int64_t>(*v, 1024, static_cast<std::int64_t>(kMaxLogBytesCeiling)));
  }
  if (auto v = setting_number(raw, "max_job_logs", &err)) {
    s.max_job_logs = static_cast<int>(std::clamp<std::int64_t>(*v, 0, 1000));
  }
  if (!err.empty()) {
    *error = err;
    return std::nullopt;
  }
  return s;
}

// ---- talking to GitLab ----

class Api {
 public:
  Api(const Settings& s, const sig::ConnectorContext& ctx) : s_(s), ctx_(ctx) {}

  bool authed() const { return ctx_.secret.has_value() && !ctx_.secret->value.empty(); }

  std::string project_path() const { return "/projects/" + s_.project_id; }

  net::HttpResponse get(const std::string& path, std::size_t max_body = 32 * kMiB,
                        std::chrono::seconds timeout = std::chrono::seconds(60)) const {
    net::HttpRequest req;
    req.url = s_.base + "/api/v4" + path;
    req.allowed_hosts = {s_.host};
    req.cancel = ctx_.cancel;
    req.max_body_bytes = max_body;
    req.timeout = timeout;
    // The only place the token goes: a header, which https_fetch writes to an
    // owner-only file rather than argv. No Accept header: the job trace
    // endpoint answers text/plain and GitLab may refuse a JSON-only Accept.
    if (authed()) req.headers.emplace_back("PRIVATE-TOKEN", ctx_.secret->value);
    if (!ctx_.transport) {
      net::HttpResponse r;
      r.error = "no transport";
      return r;
    }
    return ctx_.transport(req);
  }

 private:
  const Settings& s_;
  const sig::ConnectorContext& ctx_;
};

// GitLab's own error message ({"message": ...} or {"error": ...}), short.
// Only the reply body is quoted: it is GitLab's text, and the request -- the
// one place the token is -- is never echoed.
std::string gitlab_message(const net::HttpResponse& r) {
  json::ParseError pe;
  auto v = json::parse(r.body, &pe);
  if (!v || !v->is_object()) return {};
  std::string m;
  for (const char* k : {"message", "error"}) {
    const json::Value* f = v->find(k);
    if (f == nullptr) continue;
    m = f->is_string() ? f->as_string() : f->dump();
    break;
  }
  if (m.size() > kMaxMessageBytes) m = m.substr(0, kMaxMessageBytes) + "...";
  return m;
}

std::string describe(const net::HttpResponse& r, const std::string& what, bool authed) {
  if (!r.ok) return "could not reach GitLab for " + what + ": " + r.error;
  std::string out;
  switch (r.status) {
    case 401:
      out = authed ? "GitLab refused the token (401) for " + what +
                         ": it may be expired or revoked, and it needs the read_api scope"
                   : "GitLab asks for sign-in (401) for " + what +
                         ": the project is not public; set GITLAB_TOKEN or the Keychain item "
                         "com.devx.gitlab (read_api)";
      break;
    case 403:
      out = authed ? "the token may not read " + what +
                         " (403): it needs the read_api scope and access to the project"
                   : "GitLab does not show " + what +
                         " to anonymous readers (403); a token with read_api may see it";
      break;
    case 404:
      out = "GitLab has no " + what + " (404): check base_url and the project id or path" +
            (authed ? std::string() : std::string("; a private project also answers 404 without a token"));
      break;
    case 429:
      out = "GitLab's rate limit (429) stopped the request for " + what;
      break;
    default:
      out = "GitLab answered " + std::to_string(r.status) + " for " + what;
  }
  const std::string m = gitlab_message(r);
  if (!m.empty()) out += " -- \"" + m + "\"";
  return out;
}

std::optional<json::Value> parse_body(const net::HttpResponse& r) {
  json::ParseError pe;
  return json::parse(r.body, &pe);
}

// The next page, from X-Next-Page; 0 when there is none. GitLab leaves the
// pagination headers out only for very large collections, so when the header
// is missing a full page is taken to have more behind it rather than to be
// the end.
int next_page(const net::HttpResponse& r, std::size_t count, int per_page, int page) {
  if (const auto h = r.header("x-next-page")) {
    const std::string t = trim(*h);
    if (t.empty() || !all_digits(t) || t.size() > 9) return 0;
    const int n = std::stoi(t);
    return n > page ? n : 0;
  }
  return count >= static_cast<std::size_t>(per_page) ? page + 1 : 0;
}

std::string severity_for(const std::string& status, bool allow_failure) {
  if (status == "failed") return allow_failure ? "warning" : "error";
  if (status == "canceled") return "warning";
  return "info";
}

// What GitLab says was built. The basis is exact only for a full SHA: that is
// GitLab's statement of the commit, not an inference. Only a branch is
// recorded as the branch: a tag is not one, and neither is a hidden ref such
// as `refs/merge-requests/12/merge` (a merge-request or merged-results
// pipeline, whose SHA is the merge GitLab built) or `refs/workloads/...`.
// The attributes keep the ref as GitLab gave it.
sig::ReleaseIdentity release_for(const std::optional<std::string>& sha,
                                 const std::optional<std::string>& ref, bool tag,
                                 sig::EvidenceBasis* basis) {
  sig::ReleaseIdentity rel;
  rel.source = sig::FactSource::kProvider;
  *basis = sig::EvidenceBasis::kUnknown;
  if (sha) {
    const std::string s = lower(*sha);
    if (sig::looks_like_sha(s)) {
      rel.commit_sha = s;
      *basis = s.size() >= 40 ? sig::EvidenceBasis::kExact : sig::EvidenceBasis::kCandidate;
    }
  }
  if (ref && !tag && ref->rfind("refs/", 0) != 0) rel.branch = *ref;
  return rel;
}

// ---- sync ----

struct PipelineFacts {
  std::int64_t id = 0;
  std::optional<std::string> sha, ref, created_at, web_url;
  bool tag = false;
};

class Syncer {
 public:
  Syncer(const Settings& s, const sig::ConnectorConfig& config, const sig::SyncCursor& cursor,
         sig::SignalSink& sink, const sig::ConnectorContext& ctx)
      : s_(s), config_(config), cursor_(cursor), sink_(sink), ctx_(ctx), api_(s, ctx) {}

  sig::SyncResult run();

 private:
  bool cancelled() const { return ctx_.cancel.cancelled(); }

  // Each returns false when the sync must stop; the reason is recorded.
  bool stop(const std::string& why) {
    r_.error = why;
    stopped_ = true;
    return false;
  }
  bool bound(const std::string& why) {
    r_.notes.push_back(why);
    stopped_ = true;
    return false;
  }
  bool fail(const net::HttpResponse& resp, const std::string& what) {
    if (resp.ok && resp.status == 429) {
      r_.rate_limited = true;
      r_.retry_after_s = net::retry_after_seconds(resp);
    }
    if (cancelled()) return stop("cancelled");
    return stop(describe(resp, what, api_.authed()));
  }

  bool pipelines();
  bool pipeline(const json::Value& item);
  bool jobs(const PipelineFacts& p);
  bool job(const PipelineFacts& p, const json::Value& j);
  bool attach_log(std::int64_t job_id, sig::SignalRecord& rec);
  bool tests(const PipelineFacts& p);
  bool deployments();
  bool deployment(const json::Value& d);

  sig::SignalRecord record(const char* kind, const std::string& external_id) const {
    sig::SignalRecord rec;
    rec.workspace_id = ctx_.workspace_id;
    rec.provider = kProvider;
    rec.connector_id = config_.id;
    rec.kind = kind;
    rec.external_id = external_id;
    rec.id = sig::make_signal_id(config_.id, external_id);
    rec.observed_at = ctx_.now_iso;
    return rec;
  }
  bool emit(sig::SignalRecord rec) {
    if (r_.records_written >= ctx_.max_records) {
      return bound("stopped at max_records (" + std::to_string(ctx_.max_records) +
                   "); the next sync continues from here");
    }
    std::string err;
    const std::string what = rec.external_id;
    if (!sink_.put_signal(std::move(rec), &err)) {
      r_.notes.push_back(what + " was not stored: " + err);
      return true;
    }
    r_.records_written++;
    return true;
  }
  std::string keep_raw(const std::string& category, const std::string& name, const std::string& bytes) {
    const std::string ref = sink_.put_raw(kProvider, category, name, bytes);
    if (ref.empty()) {
      r_.notes.push_back("raw evidence " + category + "/" + name + " was not stored");
    } else {
      r_.raw_written++;
    }
    return ref;
  }
  void see_updated(const json::Value& o) {
    const auto u = str_of(o, "updated_at");
    if (!u) return;
    const auto t = sig::parse_iso8601(*u);
    if (t && (!newest_ || *t > *newest_)) {
      newest_ = *t;
      newest_text_ = *u;
    }
  }
  std::string list_query(int per_page, int page) const {
    return "?updated_after=" + net::url_encode(since_) + "&order_by=updated_at&sort=desc&per_page=" +
           std::to_string(per_page) + "&page=" + std::to_string(page);
  }

  const Settings& s_;
  const sig::ConnectorConfig& config_;
  const sig::SyncCursor& cursor_;
  sig::SignalSink& sink_;
  const sig::ConnectorContext& ctx_;
  Api api_;

  sig::SyncResult r_;
  bool stopped_ = false;
  int resume_page_ = 0;           // pipelines page to resume from; 0 = start over
  std::string since_;
  std::optional<std::int64_t> newest_;
  std::string newest_text_;
  int pipelines_done_ = 0;
  int logs_fetched_ = 0;
  std::size_t log_bytes_ = 0;
  int logs_over_count_ = 0, logs_over_budget_ = 0, tests_unreadable_ = 0;
};

sig::SyncResult Syncer::run() {
  r_.started_at = ctx_.now_iso;
  r_.cursor = cursor_;
  int start_page = 1;
  // A partial sync left a page to resume from, and the window it was paging
  // through; resuming with a different window would page a different list.
  if (all_digits(cursor_.resume) && cursor_.resume.size() < 9) {
    start_page = std::max(1, std::stoi(cursor_.resume));
    if (const auto s = str_of(cursor_.extra, "since")) since_ = *s;
  }
  if (since_.empty()) since_ = cursor_.watermark;
  if (since_.empty()) {
    auto now = sig::parse_iso8601(ctx_.now_iso);
    if (!now) now = sig::parse_iso8601(time_util::now_iso8601_utc());
    since_ = sig::format_iso8601(now.value_or(0) - static_cast<std::int64_t>(s_.lookback_days) * 86400);
  }
  resume_page_ = start_page;

  if (pipelines() && deployments()) {
    r_.status = sig::SyncStatus::kComplete;
    // Only a complete sync moves the watermark, and only to a time GitLab
    // itself reported.
    if (newest_) {
      const auto old = sig::parse_iso8601(cursor_.watermark);
      if (!old || *newest_ > *old) r_.cursor.watermark = newest_text_;
    }
    r_.cursor.resume.clear();
    r_.cursor.extra = json::Value::object();
  } else if (r_.pages == 0) {
    r_.status = sig::SyncStatus::kFailed;
    if (r_.error.empty() && !r_.notes.empty()) r_.error = r_.notes.back();
  } else {
    // Kept what was written; the watermark stays where it was so nothing
    // older than what was missed can be skipped.
    r_.status = sig::SyncStatus::kPartial;
    r_.cursor.resume = resume_page_ > 0 ? std::to_string(resume_page_) : std::string();
    json::Value extra = json::Value::object();
    if (resume_page_ > 0) extra.set("since", json::Value::string(since_));
    r_.cursor.extra = std::move(extra);
  }
  if (logs_over_count_ > 0) {
    r_.notes.push_back(std::to_string(logs_over_count_) + " failed job log(s) not fetched: max_job_logs (" +
                       std::to_string(s_.max_job_logs) + ") reached");
  }
  if (logs_over_budget_ > 0) {
    r_.notes.push_back(std::to_string(logs_over_budget_) +
                       " failed job log(s) not fetched: this sync's raw evidence budget was used up");
  }
  if (tests_unreadable_ > 0) {
    r_.notes.push_back(std::to_string(tests_unreadable_) +
                       " pipeline(s) had no readable test report summary");
  }
  return r_;
}

bool Syncer::pipelines() {
  int page = resume_page_;
  int pages = 0;
  // A page never holds more than one sync may read, so the cap falls on a
  // page boundary and the resume page moves on. With 50 per page and a cap
  // of 5, the next sync resumed from the same page and re-read the same five
  // pipelines forever (seen live against gitlab-org/cli).
  const int per_page = std::min(kPipelinesPerPage, s_.max_pipelines);
  for (;;) {
    resume_page_ = page;
    if (cancelled()) return stop("cancelled");
    if (pages >= ctx_.max_pages) {
      return bound("stopped at max_pages (" + std::to_string(ctx_.max_pages) +
                   ") of pipelines; the next sync continues from page " + std::to_string(page));
    }
    std::string path = api_.project_path() + "/pipelines" + list_query(per_page, page);
    if (!s_.ref.empty()) path += "&ref=" + net::url_encode(s_.ref);
    const net::HttpResponse resp = api_.get(path);
    if (!resp.success()) return fail(resp, "the pipelines of project " + s_.project);
    r_.pages++;
    pages++;
    const auto doc = parse_body(resp);
    if (!doc || !doc->is_array()) return stop("GitLab's pipeline list was not a JSON array");
    const int next = next_page(resp, doc->items().size(), per_page, page);
    for (const auto& item : doc->items()) {
      if (pipelines_done_ >= s_.max_pipelines) {
        return bound("stopped at max_pipelines (" + std::to_string(s_.max_pipelines) +
                     "); older pipelines in the window were not read yet");
      }
      if (!pipeline(item)) return false;
      pipelines_done_++;
    }
    if (next == 0) return true;
    if (pipelines_done_ >= s_.max_pipelines) {
      resume_page_ = next;
      return bound("stopped at max_pipelines (" + std::to_string(s_.max_pipelines) +
                   "); older pipelines in the window were not read yet");
    }
    page = next;
  }
}

bool Syncer::pipeline(const json::Value& item) {
  const auto id = int_of(item, "id");
  if (!id) {
    r_.notes.push_back("a listed pipeline without an id was skipped");
    return true;
  }
  if (cancelled()) return stop("cancelled");
  see_updated(item);
  const std::string ps = std::to_string(*id);
  const net::HttpResponse resp = api_.get(api_.project_path() + "/pipelines/" + ps);
  if (resp.ok && resp.status == 404) {
    r_.notes.push_back("pipeline " + ps + " was listed but is gone (404); skipped");
    return true;
  }
  if (!resp.success()) return fail(resp, "pipeline " + ps);
  const auto doc = parse_body(resp);
  if (!doc || !doc->is_object()) return stop("GitLab's reply for pipeline " + ps + " was not a JSON object");
  const json::Value& d = *doc;
  see_updated(d);

  PipelineFacts p;
  p.id = *id;
  p.sha = str_of(d, "sha");
  p.ref = str_of(d, "ref");
  p.tag = bool_of(d, "tag");
  p.created_at = str_of(d, "created_at");
  p.web_url = str_of(d, "web_url");
  const std::string status = str_of(d, "status").value_or("unknown");

  // The raw reply keeps everything, `user` included: it stays on this Mac.
  // The record carries only the fields below, so no person's name or avatar
  // travels with it into indexes, excerpts or evidence packs.
  sig::SignalRecord rec = record("pipeline", "pipeline:" + ps);
  rec.raw_ref = keep_raw("pipelines", ps + ".json", resp.body);
  rec.occurred_at = p.created_at.value_or("");
  rec.severity = severity_for(status, false);
  rec.title = p.ref.value_or("?") + " pipeline #" + ps + ": " + status;
  rec.release = release_for(p.sha, p.ref, p.tag, &rec.basis);
  json::Value a = json::Value::object();
  a.set("pipeline_id", json::Value::integer(p.id));
  a.set("status", json::Value::string(status));
  set_str(a, "ref", p.ref);
  a.set("tag", json::Value::boolean(p.tag));
  copy_scalar(a, "source", d, "source");
  copy_scalar(a, "duration", d, "duration");
  copy_scalar(a, "started_at", d, "started_at");
  copy_scalar(a, "finished_at", d, "finished_at");
  copy_scalar(a, "yaml_errors", d, "yaml_errors");
  set_str(a, "provider_url", p.web_url);
  rec.attributes = std::move(a);
  if (!emit(std::move(rec))) return false;
  return jobs(p) && tests(p);
}

bool Syncer::jobs(const PipelineFacts& p) {
  const std::string ps = std::to_string(p.id);
  int page = 1;
  for (int pages = 0;; pages++) {
    if (cancelled()) return stop("cancelled");
    if (pages >= ctx_.max_pages) {
      // Not a stop: a pipeline with thousands of jobs would otherwise hold
      // every later sync on the same page forever. Said, not hidden.
      r_.notes.push_back("pipeline " + ps + " has more jobs than max_pages (" +
                         std::to_string(ctx_.max_pages) + ") pages hold; the rest were not read");
      return true;
    }
    const net::HttpResponse resp = api_.get(api_.project_path() + "/pipelines/" + ps +
                                            "/jobs?per_page=" + std::to_string(kJobsPerPage) +
                                            "&page=" + std::to_string(page));
    if (!resp.success()) return fail(resp, "the jobs of pipeline " + ps);
    r_.pages++;
    const auto doc = parse_body(resp);
    if (!doc || !doc->is_array()) return stop("GitLab's job list for pipeline " + ps + " was not a JSON array");
    for (const auto& j : doc->items()) {
      if (!job(p, j)) return false;
    }
    const int next = next_page(resp, doc->items().size(), kJobsPerPage, page);
    if (next == 0) return true;
    page = next;
  }
}

bool Syncer::job(const PipelineFacts& p, const json::Value& j) {
  const auto id = int_of(j, "id");
  if (!id) {
    r_.notes.push_back("a job of pipeline " + std::to_string(p.id) + " without an id was skipped");
    return true;
  }
  const std::string js = std::to_string(*id);
  const std::string status = str_of(j, "status").value_or("unknown");
  const std::string stage = str_of(j, "stage").value_or("?");
  const std::string name = str_of(j, "name").value_or("?");
  const bool allow_failure = bool_of(j, "allow_failure");
  std::optional<std::string> sha = p.sha;
  if (const json::Value* c = obj_of(j, "commit")) {
    if (auto s = str_of(*c, "id")) sha = s;
  }

  sig::SignalRecord rec = record("ci_job", "job:" + js);
  rec.occurred_at = str_of(j, "created_at").value_or(p.created_at.value_or(""));
  rec.severity = severity_for(status, allow_failure);
  rec.title = stage + "/" + name + ": " + status;
  rec.release = release_for(sha, p.ref, p.tag, &rec.basis);
  json::Value a = json::Value::object();
  a.set("pipeline_id", json::Value::integer(p.id));
  a.set("job_id", json::Value::integer(*id));
  a.set("stage", json::Value::string(stage));
  a.set("job", json::Value::string(name));
  a.set("status", json::Value::string(status));
  copy_scalar(a, "failure_reason", j, "failure_reason");
  a.set("allow_failure", json::Value::boolean(allow_failure));
  copy_scalar(a, "duration", j, "duration");
  copy_scalar(a, "started_at", j, "started_at");
  copy_scalar(a, "finished_at", j, "finished_at");
  copy_scalar(a, "provider_url", j, "web_url");
  rec.attributes = std::move(a);
  if (status == "failed" && s_.fetch_logs && !attach_log(*id, rec)) return false;
  return emit(std::move(rec));
}

bool Syncer::attach_log(std::int64_t job_id, sig::SignalRecord& rec) {
  const std::string js = std::to_string(job_id);
  if (logs_fetched_ >= s_.max_job_logs) {
    logs_over_count_++;
    return true;
  }
  const std::size_t remaining = ctx_.max_raw_bytes > log_bytes_ ? ctx_.max_raw_bytes - log_bytes_ : 0;
  if (remaining == 0) {
    logs_over_budget_++;
    return true;
  }
  if (cancelled()) return stop("cancelled");
  const std::size_t limit = std::min(s_.max_log_bytes, remaining);
  const net::HttpResponse resp =
      api_.get(api_.project_path() + "/jobs/" + js + "/trace", limit, std::chrono::seconds(300));
  if (resp.ok && (resp.status == 403 || resp.status == 404 || resp.status == 410)) {
    // Erased, expired or hidden: permanent, so a note rather than a retry.
    r_.notes.push_back("the log of job " + js + " is not readable (" + std::to_string(resp.status) + ")");
    return true;
  }
  if (!resp.success()) return fail(resp, "the log of job " + js);
  logs_fetched_++;
  log_bytes_ += resp.body.size();
  if (resp.truncated) {
    rec.attributes.set("log_truncated", json::Value::boolean(true));
    r_.notes.push_back("the log of job " + js + " was cut at " + std::to_string(limit) +
                       " bytes; a failure near its end may be missing from root_error");
  }
  rec.raw_ref = keep_raw("jobs", js + ".log", resp.body);
  // Records stay small: the excerpt is not copied in. It is cut from the raw
  // log on demand (excerpt_lines) using the range kept here.
  const FailureSummary f = find_failure(resp.body);
  auto n = [](std::size_t v) { return json::Value::integer(static_cast<std::int64_t>(v)); };
  rec.attributes.set("log_lines", n(f.total_lines));
  rec.attributes.set("failure_candidates", n(f.candidates.size()));
  if (f.found) {
    rec.attributes.set("root_error", json::Value::string(f.root_error));
    rec.attributes.set("root_rule", json::Value::string(f.root_rule));
    rec.attributes.set("root_line", n(f.root_line));
    rec.attributes.set("excerpt_start", n(f.excerpt_start));
    rec.attributes.set("excerpt_end", n(f.excerpt_end));
  }
  return true;
}

bool Syncer::tests(const PipelineFacts& p) {
  const std::string ps = std::to_string(p.id);
  if (cancelled()) return stop("cancelled");
  const net::HttpResponse resp =
      api_.get(api_.project_path() + "/pipelines/" + ps + "/test_report_summary");
  if (resp.ok && (resp.status == 401 || resp.status == 403 || resp.status == 404)) {
    tests_unreadable_++;
    return true;
  }
  if (!resp.success()) return fail(resp, "the test report summary of pipeline " + ps);
  const auto doc = parse_body(resp);
  if (!doc || !doc->is_object()) {
    tests_unreadable_++;
    return true;
  }
  const json::Value* total = obj_of(*doc, "total");
  const auto count = total != nullptr ? int_of(*total, "count") : std::nullopt;
  if (!count || *count <= 0) return true;  // no test reports: no test record
  const std::int64_t failed = int_of(*total, "failed").value_or(0);
  const std::int64_t errors = int_of(*total, "error").value_or(0);

  sig::SignalRecord rec = record("test", "tests:" + ps);
  rec.raw_ref = keep_raw("tests", ps + "-summary.json", resp.body);
  rec.occurred_at = p.created_at.value_or("");
  rec.severity = failed + errors > 0 ? "error" : "info";
  rec.title = p.ref.value_or("?") + " pipeline #" + ps + " tests: " + std::to_string(failed) +
              " failed, " + std::to_string(errors) + " errors of " + std::to_string(*count);
  rec.release = release_for(p.sha, p.ref, p.tag, &rec.basis);
  json::Value a = json::Value::object();
  a.set("pipeline_id", json::Value::integer(p.id));
  for (const char* k : {"count", "success", "failed", "skipped", "error"}) {
    if (auto v = int_of(*total, k)) a.set(std::string(k) == "count" ? "total" : k, json::Value::integer(*v));
  }
  copy_scalar(a, "time", *total, "time");
  // Suites with failures first, so the twenty kept are the ones that matter.
  if (const json::Value* suites = doc->find("test_suites"); suites != nullptr && suites->is_array()) {
    std::vector<const json::Value*> order;
    for (const auto& s : suites->items()) {
      if (s.is_object()) order.push_back(&s);
    }
    std::stable_sort(order.begin(), order.end(), [](const json::Value* x, const json::Value* y) {
      const auto bad = [](const json::Value* s) {
        return int_of(*s, "failed_count").value_or(0) + int_of(*s, "error_count").value_or(0);
      };
      return (bad(x) > 0) && (bad(y) == 0);
    });
    json::Value list = json::Value::array();
    for (std::size_t i = 0; i < order.size() && i < kMaxSuites; i++) {
      json::Value one = json::Value::object();
      copy_scalar(one, "name", *order[i], "name");
      copy_scalar(one, "total", *order[i], "total_count");
      copy_scalar(one, "failed", *order[i], "failed_count");
      copy_scalar(one, "error", *order[i], "error_count");
      list.push_back(std::move(one));
    }
    a.set("suites", std::move(list));
    a.set("suites_total", json::Value::integer(static_cast<std::int64_t>(order.size())));
  }
  if (p.web_url) a.set("provider_url", json::Value::string(*p.web_url + "/test_report"));
  rec.attributes = std::move(a);
  return emit(std::move(rec));
}

bool Syncer::deployments() {
  // Pipelines are done; a failure from here on restarts the next sync from
  // its first pipeline page, which re-reads (and overwrites) the same records.
  resume_page_ = 0;
  int page = 1;
  for (int pages = 0;; pages++) {
    if (cancelled()) return stop("cancelled");
    if (pages >= ctx_.max_pages) {
      return bound("stopped at max_pages (" + std::to_string(ctx_.max_pages) +
                   ") of deployments; older deployments in the window were not read");
    }
    const net::HttpResponse resp =
        api_.get(api_.project_path() + "/deployments" + list_query(kDeploymentsPerPage, page));
    if (resp.ok && (resp.status == 401 || resp.status == 403 || resp.status == 404)) {
      // Deployments can be hidden from a reader who sees pipelines; that is
      // a standing limit of this credential, not a transient failure.
      r_.notes.push_back("deployments are not readable with " +
                         std::string(api_.authed() ? "this token" : "anonymous access") + " (" +
                         std::to_string(resp.status) + "); none were synced");
      return true;
    }
    if (!resp.success()) return fail(resp, "the deployments of project " + s_.project);
    r_.pages++;
    const auto doc = parse_body(resp);
    if (!doc || !doc->is_array()) return stop("GitLab's deployment list was not a JSON array");
    for (const auto& d : doc->items()) {
      if (!deployment(d)) return false;
    }
    const int next = next_page(resp, doc->items().size(), kDeploymentsPerPage, page);
    if (next == 0) return true;
    page = next;
  }
}

bool Syncer::deployment(const json::Value& d) {
  const auto id = int_of(d, "id");
  if (!id) {
    r_.notes.push_back("a deployment without an id was skipped");
    return true;
  }
  see_updated(d);
  const std::string ds = std::to_string(*id);
  const std::string status = str_of(d, "status").value_or("unknown");
  std::optional<std::string> env;
  if (const json::Value* e = obj_of(d, "environment")) env = str_of(*e, "name");
  const json::Value* job = obj_of(d, "deployable");
  const bool tag = job != nullptr && bool_of(*job, "tag");

  sig::SignalRecord rec = record("deploy", "deploy:" + ds);
  rec.occurred_at = str_of(d, "finished_at").value_or(str_of(d, "created_at").value_or(""));
  rec.environment = env;
  rec.severity = severity_for(status, false);
  const std::string number = int_of(d, "iid") ? std::to_string(*int_of(d, "iid")) : ds;
  rec.title = env.value_or("?") + " deploy #" + number + ": " + status;
  rec.release = release_for(str_of(d, "sha"), str_of(d, "ref"), tag, &rec.basis);
  rec.release.environment = env;
  json::Value a = json::Value::object();
  a.set("deployment_id", json::Value::integer(*id));
  a.set("status", json::Value::string(status));
  set_str(a, "environment", env);
  copy_scalar(a, "ref", d, "ref");
  if (job != nullptr) {
    if (auto jid = int_of(*job, "id")) a.set("deployable_job_id", json::Value::integer(*jid));
    copy_scalar(a, "provider_url", *job, "web_url");
    if (const json::Value* pl = obj_of(*job, "pipeline")) {
      if (auto pid = int_of(*pl, "id")) a.set("pipeline_id", json::Value::integer(*pid));
    }
  }
  rec.attributes = std::move(a);
  return emit(std::move(rec));
}

// ---- the connector ----

class GitLabConnector : public sig::SignalConnector {
 public:
  sig::ConnectorInfo info() const override {
    sig::ConnectorInfo i;
    i.provider = kProvider;
    i.display_name = "GitLab CI";
    i.category = "ci_cd";
    i.egress = "API requests to your GitLab (no local sessions, source or files)";
    i.credential_help =
        "GITLAB_TOKEN or the Keychain item com.devx.gitlab (read_api); optional for public projects";
    i.credential_env = "GITLAB_TOKEN";
    i.credential_service = "com.devx.gitlab";
    i.credential_optional = true;
    i.settings = {
        {"base_url", "Your GitLab, https only (default https://gitlab.com)"},
        {"project", "Numeric project id, or its path such as group/name"},
        {"ref", "Only pipelines of this branch or tag (optional)"},
        {"lookback_days", "How far back the first sync reads (default 14)"},
        {"max_pipelines", "Pipelines read per sync (default 50)"},
        {"fetch_failed_job_logs", "Keep the log of each failed job and find its root error (default true)"},
        {"max_log_bytes", "Largest job log kept, in bytes (default 20 MB)"},
        {"max_job_logs", "Failed-job logs fetched per sync (default 20)"},
    };
    return i;
  }

  sig::ConnectorCapabilities capabilities() const override {
    sig::ConnectorCapabilities c;
    c.ci_pipelines = true;
    c.ci_jobs = true;
    c.deployments = true;
    c.tests = true;
    c.incremental_pull = true;
    c.artifacts = false;  // job artifacts (binaries) are never downloaded
    c.network = true;
    return c;
  }

  sig::ValidateResult validate(const sig::ConnectorConfig& config, const sig::ConnectorContext& ctx) override {
    sig::ValidateResult v;
    std::string err;
    const auto s = read_settings(config.settings, &err);
    if (!s) {
      v.error = err;
      return v;
    }
    const Api api(*s, ctx);
    if (s->project.empty()) {
      if (!api.authed()) {
        v.error = "set `project` (a numeric id or a group/name path); without a token there is "
                  "nothing else to check";
        return v;
      }
      if (!check_user(api, &v)) return v;
      v.ok = true;
      v.notes.push_back("no project set yet: discover lists the projects this token can read");
      return v;
    }
    const net::HttpResponse resp = api.get(api.project_path());
    if (!resp.success()) {
      v.error = describe(resp, "project " + s->project, api.authed());
      return v;
    }
    const auto doc = parse_body(resp);
    if (!doc || !doc->is_object()) {
      v.error = "GitLab's reply for project " + s->project + " was not a JSON object";
      return v;
    }
    const std::string path = str_of(*doc, "path_with_namespace").value_or(s->project);
    const std::string visibility = str_of(*doc, "visibility").value_or("not reported");
    std::string note = "project " + path;
    if (auto id = int_of(*doc, "id")) note += " (id " + std::to_string(*id) + ")";
    note += ", visibility " + visibility;
    v.notes.push_back(note);
    // A project can be visible while its pipelines are not ("public
    // pipelines" off): checking here saves a sync that could only fail.
    const net::HttpResponse pl = api.get(api.project_path() + "/pipelines?per_page=1");
    if (!pl.success()) {
      v.error = describe(pl, "the pipelines of project " + path, api.authed());
      return v;
    }
    if (api.authed()) {
      if (!check_user(api, &v)) return v;
    } else {
      v.notes.push_back("no token: reading anonymously, which reaches public projects with "
                        "public pipelines only");
    }
    v.ok = true;
    return v;
  }

  sig::DiscoverResult discover(const sig::ConnectorConfig& config, const sig::ConnectorContext& ctx) override {
    sig::DiscoverResult d;
    std::string err;
    const auto s = read_settings(config.settings, &err);
    if (!s) {
      d.error = err;
      return d;
    }
    const Api api(*s, ctx);
    if (!api.authed()) {
      d.error = "listing projects needs a token (GITLAB_TOKEN or the Keychain item com.devx.gitlab, "
                "read_api); for a public project, enter its path (group/name) or numeric id as "
                "`project` instead";
      return d;
    }
    const int max_pages = std::min(ctx.max_pages, 10);
    int page = 1;
    for (int pages = 0; pages < max_pages; pages++) {
      if (ctx.cancel.cancelled()) {
        d.error = "cancelled";
        return d;
      }
      const net::HttpResponse resp =
          api.get("/projects?membership=true&simple=true&per_page=" + std::to_string(kProjectsPerPage) +
                  "&page=" + std::to_string(page));
      if (!resp.success()) {
        d.error = describe(resp, "the project list", true);
        return d;
      }
      const auto doc = parse_body(resp);
      if (!doc || !doc->is_array()) {
        d.error = "GitLab's project list was not a JSON array";
        return d;
      }
      for (const auto& p : doc->items()) {
        const auto id = int_of(p, "id");
        const auto name = str_of(p, "path_with_namespace");
        if (!id || !name) continue;
        json::Value one = json::Value::object();
        one.set("id", json::Value::string(std::to_string(*id)));
        one.set("name", json::Value::string(*name));
        one.set("kind", json::Value::string("project"));
        d.resources.push_back(std::move(one));
      }
      const int next = next_page(resp, doc->items().size(), kProjectsPerPage, page);
      if (next == 0) break;
      page = next;
    }
    d.ok = true;
    return d;
  }

  sig::SyncResult sync(const sig::ConnectorConfig& config, const sig::SyncCursor& cursor,
                       sig::SignalSink& sink, const sig::ConnectorContext& ctx) override {
    std::string err;
    const auto s = read_settings(config.settings, &err);
    if (!s || s->project.empty()) {
      sig::SyncResult r;
      r.started_at = ctx.now_iso;
      r.cursor = cursor;
      r.status = sig::SyncStatus::kFailed;
      r.error = s ? "set `project` (a numeric id or a group/name path) before syncing" : err;
      return r;
    }
    Syncer syncer(*s, config, cursor, sink, ctx);
    return syncer.run();
  }

 private:
  static bool check_user(const Api& api, sig::ValidateResult* v) {
    const net::HttpResponse resp = api.get("/user");
    if (!resp.success()) {
      v->error = describe(resp, "the token's user", true);
      return false;
    }
    const auto doc = parse_body(resp);
    if (doc && doc->is_object()) v->account = str_of(*doc, "username").value_or("");
    return true;
  }
};

}  // namespace

std::unique_ptr<signals::SignalConnector> make_gitlab_connector() {
  return std::make_unique<GitLabConnector>();
}

}  // namespace mpi::intelligence
