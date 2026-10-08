#include "adapters/sentry/sentry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mpi::intelligence {
namespace {

using signals::ConnectorConfig;
using signals::ConnectorContext;
using signals::EvidenceBasis;
using signals::FactSource;
using signals::ReleaseIdentity;
using signals::SignalRecord;
using signals::SyncCursor;
using signals::SyncResult;
using signals::SyncStatus;

constexpr const char* kProvider = "sentry";
constexpr const char* kDefaultBase = "https://sentry.io";
// Frames kept per issue: enough to see the crash site and how it got there,
// without copying a whole native stack into every record.
constexpr std::size_t kMaxFrames = 30;
constexpr std::size_t kMaxDetail = 200;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// A non-empty string member, or nullptr: "" and null are absent, never a value.
const std::string* str_field(const json::Value& v, std::string_view key) {
  const json::Value* f = v.find(key);
  if (f == nullptr || !f->is_string() || f->as_string().empty()) return nullptr;
  return &f->as_string();
}

std::optional<std::string> opt_str(const json::Value& v, std::string_view key) {
  if (const std::string* s = str_field(v, key)) return *s;
  return std::nullopt;
}

// Sentry sends some counts as strings ("count": "1423") and others as numbers.
std::optional<std::int64_t> int_field(const json::Value& v, std::string_view key) {
  const json::Value* f = v.find(key);
  if (f == nullptr) return std::nullopt;
  if (f->is_int()) return f->as_int();
  if (f->is_string()) {
    const std::string& s = f->as_string();
    if (s.empty() || s.size() > 18 ||
        !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
      return std::nullopt;
    }
    return std::stoll(s);
  }
  return std::nullopt;
}

const json::Value* obj_field(const json::Value& v, std::string_view key) {
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_object() ? f : nullptr;
}

const json::Value* arr_field(const json::Value& v, std::string_view key) {
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_array() ? f : nullptr;
}

// The first of several spellings: Sentry's API serializes frames in camelCase
// (`lineNo`, `inApp`, `absPath`), event payloads and older servers in
// snake_case.
const json::Value* first_of(const json::Value& v, std::initializer_list<std::string_view> keys) {
  for (const auto k : keys) {
    const json::Value* f = v.find(k);
    if (f != nullptr && !f->is_null()) return f;
  }
  return nullptr;
}

// Seconds for comparing provider times; nullopt for a time that does not
// parse, which then never moves a watermark.
std::optional<std::int64_t> epoch(const std::string& iso) { return signals::parse_iso8601(iso); }

// A raw file name part: [A-Za-z0-9._-], bounded so the ".json" survives the
// store's own length cap.
std::string file_part(const std::string& s) {
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    out.push_back(ok ? c : '_');
    if (out.size() >= 100) break;
  }
  if (!out.empty() && out[0] == '.') out[0] = '_';
  return out.empty() ? "_" : out;
}

std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

// make_signal_id cuts at 64 characters, so two long release versions that
// differ only at the end ("...+54019" / "...+54020") would share a file. A
// long id is replaced by a hash of the external id: still stable, never
// shared.
std::string signal_id(const std::string& connector_id, const std::string& kind,
                      const std::string& external_id) {
  std::string id = signals::make_signal_id(connector_id, external_id);
  if (id.size() < 64) return id;
  static const char* kHex = "0123456789abcdef";
  std::uint64_t h = fnv1a(external_id);
  std::string hex(16, '0');
  for (int i = 15; i >= 0; i--) {
    hex[static_cast<std::size_t>(i)] = kHex[h & 15u];
    h >>= 4;
  }
  return signals::make_signal_id(connector_id, kind + "_" + hex);
}

// The decoded `cursor` query parameter of a Sentry URL; empty when absent.
std::string cursor_param(const std::string& url) {
  const auto q = url.find('?');
  if (q == std::string::npos) return {};
  std::size_t pos = q + 1;
  while (pos <= url.size()) {
    auto amp = url.find('&', pos);
    if (amp == std::string::npos) amp = url.size();
    const std::string pair = url.substr(pos, amp - pos);
    if (pair.rfind("cursor=", 0) == 0) {
      const std::string enc = pair.substr(7);
      std::string out;
      for (std::size_t i = 0; i < enc.size(); i++) {
        if (enc[i] == '%' && i + 2 < enc.size() && std::isxdigit(static_cast<unsigned char>(enc[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(enc[i + 2]))) {
          out.push_back(static_cast<char>(std::stoi(enc.substr(i + 1, 2), nullptr, 16)));
          i += 2;
        } else {
          out.push_back(enc[i] == '+' ? ' ' : enc[i]);
        }
      }
      return out;
    }
    pos = amp + 1;
  }
  return {};
}

// The exact bytes of each element of a top-level JSON array, so the raw store
// keeps every issue and release as Sentry sent it rather than as DevX
// re-serialized it. Empty for anything that is not a well-formed array; the
// caller then falls back to the parsed value.
std::vector<std::string_view> array_elements(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  auto ws = [&] {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) i++;
  };
  ws();
  if (i >= text.size() || text[i] != '[') return {};
  i++;
  ws();
  if (i < text.size() && text[i] == ']') return out;
  while (i < text.size()) {
    ws();
    const std::size_t start = i;
    int depth = 0;
    bool in_str = false;
    for (; i < text.size(); i++) {
      const char c = text[i];
      if (in_str) {
        if (c == '\\') {
          i++;
        } else if (c == '"') {
          in_str = false;
        }
        continue;
      }
      if (c == '"') {
        in_str = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        if (depth == 0) break;
        depth--;
      } else if (c == ',' && depth == 0) {
        break;
      }
    }
    if (i >= text.size() || depth != 0) return {};
    std::size_t end = i;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\r' ||
                           text[end - 1] == '\t')) {
      end--;
    }
    if (end == start) return {};
    out.push_back(text.substr(start, end - start));
    if (text[i] == ']') return out;
    i++;  // ','
  }
  return {};
}

// ---- settings ----

struct Settings {
  std::string base;  // https://host[/prefix], no trailing slash
  std::string host;
  std::string org, project;
  std::optional<std::string> environment;
  int lookback_days = 14;
  int max_issues = 200;
  int max_latest_events = 50;
};

std::optional<std::string> setting_str(const json::Value& s, const char* key) {
  const json::Value* v = s.find(key);
  if (v == nullptr || v->is_null()) return std::nullopt;
  if (v->is_string()) {
    if (v->as_string().empty()) return std::nullopt;
    return v->as_string();
  }
  if (v->is_int()) return std::to_string(v->as_int());
  return std::nullopt;
}

// A whole number within [lo, hi]; a value that is there but is not one is an
// error rather than quietly replaced by the default.
bool setting_int(const json::Value& s, const char* key, int lo, int hi, int* out, std::string* error) {
  const auto v = setting_str(s, key);
  if (!v) return true;
  if (v->size() > 9 || !std::all_of(v->begin(), v->end(), [](unsigned char c) { return std::isdigit(c); })) {
    *error = std::string("the Sentry setting '") + key + "' must be a whole number";
    return false;
  }
  const int n = std::stoi(*v);
  if (n < lo || n > hi) {
    *error = std::string("the Sentry setting '") + key + "' must be between " + std::to_string(lo) +
             " and " + std::to_string(hi);
    return false;
  }
  *out = n;
  return true;
}

std::optional<Settings> read_settings(const ConnectorConfig& config, bool need_project,
                                      std::string* error) {
  Settings s;
  const json::Value& v = config.settings;
  s.base = setting_str(v, "base_url").value_or(kDefaultBase);
  while (!s.base.empty() && s.base.back() == '/') s.base.pop_back();
  s.host = net::url_host(s.base);
  // A self-hosted Sentry is welcome, but only over https: the token travels
  // in a header on every request.
  if (s.host.empty() || s.base.find_first_of("?#") != std::string::npos) {
    *error = "the Sentry base_url must be an https URL such as https://sentry.io (got '" + s.base + "')";
    return std::nullopt;
  }
  s.org = setting_str(v, "organization").value_or("");
  s.project = setting_str(v, "project").value_or("");
  if (s.org.empty()) {
    *error = "the Sentry connector needs the 'organization' setting (the organization slug)";
    return std::nullopt;
  }
  if (need_project && s.project.empty()) {
    *error = "the Sentry connector needs the 'project' setting (the project slug)";
    return std::nullopt;
  }
  s.environment = setting_str(v, "environment");
  if (!setting_int(v, "lookback_days", 1, 90, &s.lookback_days, error) ||
      !setting_int(v, "max_issues", 1, 100000, &s.max_issues, error) ||
      !setting_int(v, "max_latest_events", 0, 10000, &s.max_latest_events, error)) {
    return std::nullopt;
  }
  return s;
}

// ---- requests ----

struct Failure {
  std::string message;
  bool rate_limited = false;
  std::optional<int> retry_after_s;
};

class Client {
 public:
  Client(const Settings& s, const ConnectorContext& ctx) : s_(s), ctx_(ctx) {
    if (ctx.secret) token_ = ctx.secret->value;
  }

  bool has_token() const { return !token_.empty(); }
  const Settings& settings() const { return s_; }

  std::string api(const std::string& path) const { return s_.base + "/api/0/" + path; }
  std::string org_path() const { return "organizations/" + net::url_encode(s_.org) + "/"; }
  std::string project_path() const {
    return "projects/" + net::url_encode(s_.org) + "/" + net::url_encode(s_.project) + "/";
  }

  net::HttpResponse get(const std::string& url) const {
    net::HttpRequest req;
    req.url = url;
    // The token goes here and nowhere else: https_fetch moves headers into
    // an owner-only config file, never argv.
    req.headers.emplace_back("Authorization", "Bearer " + token_);
    req.headers.emplace_back("Accept", "application/json");
    req.allowed_hosts = {s_.host};
    req.cancel = ctx_.cancel;
    net::HttpResponse r;
    if (!ctx_.transport) {
      r.error = "no transport";
      return r;
    }
    return ctx_.transport(req);
  }

  // Anything that will be shown or stored passes through here. A reply or a
  // transport error that echoes the token must not carry it any further.
  std::string scrub(std::string text) const {
    if (token_.empty()) return text;
    for (auto pos = text.find(token_); pos != std::string::npos; pos = text.find(token_, pos)) {
      text.replace(pos, token_.size(), "[redacted]");
    }
    return text;
  }

  // Nullopt for a usable 2xx reply; otherwise why it is not one, in words.
  std::optional<Failure> check(const net::HttpResponse& r, const std::string& what) const {
    if (r.success() && !r.truncated) return std::nullopt;
    Failure f;
    if (!r.ok) {
      f.message = "could not reach Sentry for " + what + ": " + r.error;
    } else if (r.truncated) {
      f.message = "Sentry's reply for " + what + " was larger than DevX reads";
    } else if (r.status == 401 || r.status == 403) {
      f.message = "Sentry refused the token (HTTP " + std::to_string(r.status) + ") for " + what +
                  ": check that it is valid and has org:read, project:read and event:read";
    } else if (r.status == 429) {
      f.rate_limited = true;
      f.retry_after_s = net::retry_after_seconds(r);
      f.message = "Sentry rate-limited " + what + " (HTTP 429)";
      if (f.retry_after_s) f.message += "; retry after " + std::to_string(*f.retry_after_s) + " s";
    } else {
      f.message = "Sentry answered HTTP " + std::to_string(r.status) + " for " + what;
    }
    // Sentry explains a refusal in {"detail": "..."}; that sentence helps,
    // the rest of the body is not shown.
    if (r.ok && !r.success()) {
      if (auto doc = json::parse(r.body, nullptr); doc && doc->is_object()) {
        if (const std::string* d = str_field(*doc, "detail")) {
          f.message += " (" + d->substr(0, kMaxDetail) + ")";
        }
      }
    }
    f.message = scrub(f.message);
    return f;
  }

 private:
  const Settings& s_;
  const ConnectorContext& ctx_;
  std::string token_;
};

std::optional<json::Value> parse_json(const std::string& body) {
  return json::parse(body, nullptr);
}

// ---- normalization ----

json::Value str_or_null(const std::optional<std::string>& s) {
  return s ? json::Value::string(*s) : json::Value::null();
}

std::optional<std::string> tag(const json::Value& event, std::string_view key) {
  const json::Value* tags = arr_field(event, "tags");
  if (tags == nullptr) return std::nullopt;
  for (const auto& t : tags->items()) {
    if (!t.is_object()) continue;
    const std::string* k = str_field(t, "key");
    if (k != nullptr && *k == key) return opt_str(t, "value");
  }
  return std::nullopt;
}

// The event's release, which the events API serializes as a release object,
// a bare string, or null.
std::optional<std::string> event_release(const json::Value& event) {
  if (const json::Value* rel = event.find("release")) {
    if (rel->is_object()) {
      if (auto v = opt_str(*rel, "version")) return v;
    } else if (rel->is_string() && !rel->as_string().empty()) {
      return rel->as_string();
    }
  }
  return tag(event, "release");
}

json::Value frame_json(const json::Value& f) {
  json::Value o = json::Value::object();
  const json::Value* file = first_of(f, {"filename", "absPath", "abs_path"});
  o.set("file", file != nullptr && file->is_string() ? *file : json::Value::null());
  const json::Value* line = first_of(f, {"lineNo", "lineno"});
  o.set("line", line != nullptr && line->is_int() ? *line : json::Value::null());
  o.set("function", str_or_null(opt_str(f, "function")));
  const json::Value* in_app = first_of(f, {"inApp", "in_app"});
  o.set("in_app", in_app != nullptr && in_app->is_bool() ? *in_app : json::Value::null());
  o.set("module", str_or_null(opt_str(f, "module")));
  return o;
}

// Crash frame first. Sentry lists a stack oldest call first, so the frame
// that crashed is the last one; it is reversed here so a reader -- or a
// model with a short budget -- sees the crash site before the run loop.
json::Value frames_crash_first(const json::Value& stacktrace) {
  json::Value out = json::Value::array();
  const json::Value* frames = arr_field(stacktrace, "frames");
  if (frames == nullptr) return out;
  const auto& items = frames->items();
  for (auto it = items.rbegin(); it != items.rend() && out.size() < kMaxFrames; ++it) {
    if (it->is_object()) out.push_back(frame_json(*it));
  }
  return out;
}

const json::Value* entry_data(const json::Value& event, std::string_view type) {
  const json::Value* entries = arr_field(event, "entries");
  if (entries == nullptr) return nullptr;
  for (const auto& e : entries->items()) {
    if (!e.is_object()) continue;
    const std::string* t = str_field(e, "type");
    if (t != nullptr && *t == type) return obj_field(e, "data");
  }
  return nullptr;
}

// The exception and its frames from an event. Of chained exceptions Sentry
// lists the one raised last at the end, so that is the one reported. A
// native crash may carry its stack on the crashed thread instead.
void add_exception(const json::Value& event, json::Value* attrs) {
  const json::Value* primary = nullptr;
  if (const json::Value* data = entry_data(event, "exception")) {
    if (const json::Value* values = arr_field(*data, "values")) {
      for (const auto& v : values->items()) {
        if (v.is_object()) primary = &v;
      }
    }
  }
  if (primary != nullptr) {
    json::Value ex = json::Value::object();
    ex.set("type", str_or_null(opt_str(*primary, "type")));
    ex.set("value", str_or_null(opt_str(*primary, "value")));
    attrs->set("exception", std::move(ex));
  }
  json::Value frames = json::Value::array();
  if (primary != nullptr) {
    if (const json::Value* st = obj_field(*primary, "stacktrace")) frames = frames_crash_first(*st);
  }
  if (frames.size() == 0) {
    if (const json::Value* data = entry_data(event, "threads")) {
      if (const json::Value* values = arr_field(*data, "values")) {
        for (const auto& t : values->items()) {
          const json::Value* crashed = t.find("crashed");
          if (crashed == nullptr || !crashed->as_bool(false)) continue;
          if (const json::Value* st = obj_field(t, "stacktrace")) frames = frames_crash_first(*st);
          break;
        }
      }
    }
  }
  if (frames.size() > 0) attrs->set("frames", std::move(frames));
}

std::optional<std::string> commit_of(const json::Value& release) {
  const json::Value* c = obj_field(release, "lastCommit");
  if (c == nullptr) return std::nullopt;
  const std::string* id = str_field(*c, "id");
  if (id == nullptr) return std::nullopt;
  std::string sha = lower(*id);
  if (!signals::looks_like_sha(sha)) return std::nullopt;
  return sha;
}

bool has_release_fact(const ReleaseIdentity& r) {
  return r.version || r.build_number || r.commit_sha || r.bundle_or_package_id;
}

// ---- the sync ----

class SyncRun {
 public:
  SyncRun(const ConnectorConfig& config, const SyncCursor& cursor, signals::SignalSink& sink,
          const ConnectorContext& ctx, const Client& client)
      : config_(config), cursor_(cursor), sink_(sink), ctx_(ctx), client_(client) {
    r_.started_at = ctx.now_iso;
    r_.cursor = cursor;
    r_.cursor.connector_id = config.id;
  }

  SyncResult run() {
    std::optional<std::string> project_id;
    if (lookup_project(&project_id) && sync_releases(project_id)) sync_issues();
    return finish();
  }

 private:
  // ---- bookkeeping ----

  bool cancelled() {
    if (!ctx_.cancel.cancelled()) return false;
    stop({"the sync was cancelled", false, std::nullopt});
    return true;
  }

  // Ends the sync early. What was written stays; finish() decides between
  // partial and failed.
  void stop(const Failure& f) {
    stopped_ = true;
    r_.error = f.message;
    if (f.rate_limited) {
      r_.rate_limited = true;
      r_.retry_after_s = f.retry_after_s;
    }
  }

  std::string raw(const std::string& category, const std::string& name, std::string_view bytes) {
    if (raw_bytes_ + bytes.size() > ctx_.max_raw_bytes) {
      raw_over_budget_++;
      return {};
    }
    std::string ref = sink_.put_raw(kProvider, category, name, std::string(bytes));
    if (ref.empty()) {
      raw_failed_++;
      return {};
    }
    raw_bytes_ += bytes.size();
    r_.raw_written++;
    return ref;
  }

  void put(SignalRecord rec) {
    std::string err;
    if (sink_.put_signal(std::move(rec), &err)) {
      r_.records_written++;
    } else {
      if (put_failed_ == 0) first_put_error_ = client_.scrub(err);
      put_failed_++;
    }
  }

  SignalRecord base_record(const std::string& kind, const std::string& external_id) const {
    SignalRecord rec;
    rec.id = signal_id(config_.id, kind, external_id);
    rec.workspace_id = ctx_.workspace_id;
    rec.provider = kProvider;
    rec.connector_id = config_.id;
    rec.kind = kind;
    rec.external_id = external_id;
    rec.observed_at = ctx_.now_iso;
    return rec;
  }

  // ---- project ----

  // The numeric id the releases endpoint filters by. A project Sentry does
  // not know, or a token it refuses, ends the sync here, before anything is
  // written.
  bool lookup_project(std::optional<std::string>* id) {
    if (cancelled()) return false;
    const auto resp = client_.get(client_.api(client_.project_path()));
    if (auto f = client_.check(resp, "project '" + client_.settings().project + "'")) {
      stop(*f);
      return false;
    }
    auto doc = parse_json(resp.body);
    if (!doc || !doc->is_object()) {
      stop({"Sentry's reply for the project was not a JSON object", false, std::nullopt});
      return false;
    }
    if (auto v = opt_str(*doc, "id")) {
      *id = *v;
    } else if (const json::Value* n = doc->find("id"); n != nullptr && n->is_int()) {
      *id = std::to_string(n->as_int());
    } else {
      r_.notes.push_back("Sentry did not name the project's numeric id, so releases were read "
                         "for the whole organization");
    }
    return true;
  }

  // ---- releases ----

  bool sync_releases(const std::optional<std::string>& project_id) {
    if (cancelled()) return false;
    std::string url = client_.api(client_.org_path() + "releases/?per_page=50");
    if (project_id) url += "&project=" + net::url_encode(*project_id);
    const auto resp = client_.get(url);
    if (auto f = client_.check(resp, "the release list")) {
      stop(*f);
      return false;
    }
    auto doc = parse_json(resp.body);
    if (!doc || !doc->is_array()) {
      stop({"Sentry's release list was not a JSON array", false, std::nullopt});
      return false;
    }
    r_.pages++;
    const auto slices = array_elements(resp.body);
    const bool exact = slices.size() == doc->items().size();
    std::size_t i = 0;
    int skipped = 0;
    for (const auto& rel : doc->items()) {
      const std::size_t idx = i++;
      if (!rel.is_object()) continue;
      const std::string* version = str_field(rel, "version");
      if (version == nullptr) {
        skipped++;
        continue;
      }
      SignalRecord rec = base_record("release", "release:" + *version);
      rec.title = *version;
      rec.occurred_at = opt_str(rel, "dateReleased").value_or(opt_str(rel, "dateCreated").value_or(""));
      rec.release = parse_sentry_release(*version);
      if (auto sha = commit_of(rel)) rec.release.commit_sha = sha;
      if (rec.release.commit_sha) commits_[*version] = *rec.release.commit_sha;
      json::Value attrs = json::Value::object();
      if (auto n = int_field(rel, "newGroups")) attrs.set("new_groups", json::Value::integer(*n));
      if (auto n = int_field(rel, "commitCount")) attrs.set("commit_count", json::Value::integer(*n));
      if (const json::Value* d = obj_field(rel, "lastDeploy")) {
        rec.release.environment = opt_str(*d, "environment");
        rec.environment = rec.release.environment;
        json::Value ld = json::Value::object();
        ld.set("environment", str_or_null(opt_str(*d, "environment")));
        ld.set("finished_at", str_or_null(opt_str(*d, "dateFinished")));
        attrs.set("last_deploy", std::move(ld));
      }
      rec.release.source = FactSource::kProvider;
      rec.basis = EvidenceBasis::kProviderAttributed;
      rec.attributes = std::move(attrs);
      const std::string bytes = exact ? std::string(slices[idx]) : rel.dump();
      rec.raw_ref = raw("releases", file_part(*version) + ".json", bytes);
      put(std::move(rec));
    }
    if (skipped > 0) {
      r_.notes.push_back(std::to_string(skipped) + " releases without a version were skipped");
    }
    // One page: the newest releases. Older ones are not read, so an issue
    // on an older release keeps its version but gets no commit from here.
    if (const auto link = resp.header("link"); link && net::next_link(*link)) {
      r_.notes.push_back("only the newest " + std::to_string(doc->items().size()) +
                         " releases were read; issues on older releases get no commit from Sentry");
    }
    return true;
  }

  // ---- issues ----

  // Whether this sync reads the whole lookback window rather than what was
  // seen since the watermark. An issue resolved or ignored without a new
  // event keeps its lastSeen, so `lastSeen:>` never returns it and its state
  // would go stale locally; reading the whole window once a day catches it.
  // A partial full read resumes as one.
  bool full_sweep() const {
    if (!epoch(cursor_.watermark)) return true;
    if (!cursor_.resume.empty()) {
      const json::Value* v = cursor_.extra.find("sweeping");
      return v != nullptr && v->as_bool(false);
    }
    const json::Value* last = cursor_.extra.find("last_full_at");
    const auto last_t = last != nullptr && last->is_string() ? epoch(last->as_string()) : std::nullopt;
    const auto now_t = epoch(ctx_.now_iso);
    return !last_t || !now_t || *now_t - *last_t >= 86400;
  }

  std::string first_issues_url() const {
    const Settings& s = client_.settings();
    // statsPeriod picks the stats Sentry attaches ("24h" or "14d"); what is
    // listed is chosen by the query. An explicit query also replaces
    // Sentry's default `is:unresolved`, so resolved and ignored issues are
    // read too and a state change is not lost.
    std::string q;
    const auto wm = epoch(cursor_.watermark);
    if (wm && !full_sweep()) {
      // Seconds, fractions dropped: the boundary second is read again,
      // which is harmless, rather than skipped.
      q = "lastSeen:>" + signals::format_iso8601(*wm);
    } else {
      q = "lastSeen:-" + std::to_string(s.lookback_days) + "d";
    }
    std::string url = client_.api(client_.project_path() + "issues/?statsPeriod=" +
                                  std::string(s.lookback_days <= 1 ? "24h" : "14d") +
                                  "&query=" + net::url_encode(q));
    if (s.environment) url += "&environment=" + net::url_encode(*s.environment);
    if (!cursor_.resume.empty()) url += "&cursor=" + net::url_encode(cursor_.resume);
    return url;
  }

  void sync_issues() {
    const Settings& s = client_.settings();
    const int limit = std::min(s.max_issues, ctx_.max_records);
    std::string url = first_issues_url();
    int pages = 0;
    int seen = 0;
    while (true) {
      if (cancelled()) {
        r_.cursor.resume = cursor_param(url);
        return;
      }
      const auto resp = client_.get(url);
      if (auto f = client_.check(resp, "the issue list")) {
        stop(*f);
        // Resume where it stopped. On the first page that is where this
        // sync started, so nothing is skipped either way.
        r_.cursor.resume = cursor_param(url);
        return;
      }
      auto doc = parse_json(resp.body);
      if (!doc || !doc->is_array()) {
        stop({"Sentry's issue list was not a JSON array", false, std::nullopt});
        r_.cursor.resume = cursor_param(url);
        return;
      }
      pages++;
      r_.pages++;
      const auto slices = array_elements(resp.body);
      const bool exact = slices.size() == doc->items().size();
      std::size_t i = 0;
      for (const auto& issue : doc->items()) {
        const std::size_t idx = i++;
        if (!issue.is_object()) continue;
        seen++;
        write_issue(issue, exact ? slices[idx] : std::string_view());
      }
      const auto link = resp.header("link");
      const auto next = link ? net::next_link(*link) : std::nullopt;
      if (!next) {
        r_.cursor.resume.clear();
        return;
      }
      const std::string next_cursor = cursor_param(*next);
      // Bounds are checked between pages, never inside one: a resume cursor
      // names a page, so stopping halfway through a page would skip the
      // rest of it. A sync can therefore write up to one page past the cap.
      std::string why;
      if (!net::url_allowed(*next, {s.host})) {
        why = "Sentry's next-page link pointed away from " + s.host + "; it was not followed";
      } else if (pages >= ctx_.max_pages) {
        why = "stopped after " + std::to_string(pages) + " pages of issues (the limit per sync)";
      } else if (seen >= limit) {
        why = "stopped after " + std::to_string(seen) + " issues (max_issues)";
      }
      if (!why.empty()) {
        incomplete_ = true;
        r_.cursor.resume = next_cursor;
        r_.notes.push_back(why + (next_cursor.empty() ? std::string() : "; the next sync continues from there"));
        return;
      }
      if (cancelled()) {
        r_.cursor.resume = next_cursor;
        return;
      }
      url = *next;
    }
  }

  void write_issue(const json::Value& issue, std::string_view exact_bytes) {
    const std::string* id = str_field(issue, "id");
    if (id == nullptr) {
      issues_without_id_++;
      return;
    }
    const std::string bytes = exact_bytes.empty() ? issue.dump() : std::string(exact_bytes);
    const std::string level = opt_str(issue, "level").value_or("");
    const json::Value* unhandled = issue.find("isUnhandled");
    const bool crash = level == "fatal" || (unhandled != nullptr && unhandled->as_bool(false));

    SignalRecord rec = base_record(crash ? "crash" : "issue", *id);
    if (!level.empty()) rec.severity = level;
    rec.title = opt_str(issue, "title");
    rec.occurred_at = opt_str(issue, "firstSeen").value_or("");
    rec.raw_ref = raw("issues", file_part(*id) + ".json", bytes);

    json::Value attrs = json::Value::object();
    auto copy_str = [&](const char* from, const char* to) {
      if (auto v = opt_str(issue, from)) attrs.set(to, json::Value::string(*v));
    };
    copy_str("shortId", "short_id");
    copy_str("culprit", "culprit");
    copy_str("status", "status");
    if (auto n = int_field(issue, "count")) attrs.set("count", json::Value::integer(*n));
    if (auto n = int_field(issue, "userCount")) attrs.set("user_count", json::Value::integer(*n));
    copy_str("firstSeen", "first_seen");
    copy_str("lastSeen", "last_seen");
    copy_str("level", "level");
    copy_str("type", "type");
    copy_str("permalink", "provider_url");
    if (unhandled != nullptr && unhandled->is_bool()) attrs.set("is_unhandled", *unhandled);

    // The latest event: the release, environment and stack the issue itself
    // does not carry.
    std::optional<json::Value> event;
    if (events_fetched_ < client_.settings().max_latest_events && !events_stopped_) {
      event = latest_event(*id, &attrs);
    } else {
      events_not_fetched_++;
    }
    std::optional<std::string> env;
    if (event) {
      env = tag(*event, "environment");
      if (!env) env = opt_str(*event, "environment");
      const auto release = event_release(*event);
      const auto dist = opt_str(*event, "dist") ? opt_str(*event, "dist") : tag(*event, "dist");
      ReleaseIdentity rel;
      if (release) {
        rel = parse_sentry_release(*release);
        attrs.set("release", json::Value::string(*release));
        if (auto it = commits_.find(*release); it != commits_.end()) rel.commit_sha = it->second;
      }
      // What the SDK reported about the app, for what the release string
      // did not say. A release string, when there is one, wins: it is what
      // Sentry groups the release by.
      if (const json::Value* contexts = obj_field(*event, "contexts")) {
        if (const json::Value* app = obj_field(*contexts, "app")) {
          if (!rel.version) rel.version = opt_str(*app, "app_version");
          if (!rel.bundle_or_package_id) rel.bundle_or_package_id = opt_str(*app, "app_identifier");
          if (!rel.build_number && !dist) rel.build_number = opt_str(*app, "app_build");
        }
      }
      if (dist) {
        attrs.set("dist", json::Value::string(*dist));
        if (!rel.build_number) rel.build_number = dist;
      }
      add_exception(*event, &attrs);
      if (has_release_fact(rel)) {
        rel.environment = env;
        rel.source = FactSource::kProvider;
        rec.release = std::move(rel);
        rec.basis = EvidenceBasis::kProviderAttributed;
      }
    }
    if (!event) carry_forward(rec, attrs, env);
    rec.environment = env ? env : client_.settings().environment;
    rec.attributes = std::move(attrs);

    if (const std::string* ls = str_field(issue, "lastSeen")) {
      const auto t = epoch(*ls);
      if (t && (!max_last_seen_t_ || *t > *max_last_seen_t_)) {
        max_last_seen_t_ = t;
        max_last_seen_ = *ls;
      }
    }
    put(std::move(rec));
  }

  // No event was read for this issue this time (beyond max_latest_events, a
  // rate limit, a failed read). What an earlier sync learnt from its event --
  // the release, the stack -- is still true of the issue, so it is kept, and
  // the record says it came from an earlier read. Without this a full read
  // would strip the release from every issue past the first fifty.
  void carry_forward(SignalRecord& rec, json::Value& attrs, std::optional<std::string>& env) {
    const auto prev = sink_.existing(rec.id);
    if (!prev) return;
    bool carried = false;
    for (const char* k : {"release", "dist", "exception", "frames", "event_raw_ref", "event_id"}) {
      if (attrs.find(k) != nullptr) continue;
      if (const json::Value* v = prev->attributes.find(k)) {
        attrs.set(k, *v);
        carried = true;
      }
    }
    if (rec.release.empty() && !prev->release.empty()) {
      rec.release = prev->release;
      rec.basis = prev->basis;
      carried = true;
    }
    if (!env && prev->environment) env = prev->environment;
    if (carried) attrs.set("event_from_earlier_sync", json::Value::boolean(true));
  }

  std::optional<json::Value> latest_event(const std::string& issue_id, json::Value* attrs) {
    if (ctx_.cancel.cancelled()) return std::nullopt;
    events_fetched_++;
    const auto resp = client_.get(client_.api(client_.org_path() + "issues/" +
                                              net::url_encode(issue_id) + "/events/latest/"));
    if (resp.ok && resp.status == 404) {
      // An issue whose events have aged out of retention: nothing to read.
      events_missing_++;
      return std::nullopt;
    }
    if (auto f = client_.check(resp, "the latest event of issue " + issue_id)) {
      // The issue is still written, without what its event would add; the
      // sync is partial so the next one reads it again.
      incomplete_ = true;
      events_failed_++;
      if (first_event_error_.empty()) first_event_error_ = f->message;
      if (f->rate_limited) {
        r_.rate_limited = true;
        r_.retry_after_s = f->retry_after_s;
        events_stopped_ = true;
      }
      return std::nullopt;
    }
    auto doc = parse_json(resp.body);
    if (!doc || !doc->is_object()) {
      incomplete_ = true;
      events_failed_++;
      if (first_event_error_.empty()) first_event_error_ = "an event reply was not a JSON object";
      return std::nullopt;
    }
    const std::string ref = raw("events", file_part(issue_id) + "-latest.json", resp.body);
    if (!ref.empty()) attrs->set("event_raw_ref", json::Value::string(ref));
    if (auto eid = opt_str(*doc, "eventID")) attrs->set("event_id", json::Value::string(*eid));
    return doc;
  }

  SyncResult finish() {
    auto count_note = [&](int n, const std::string& what) {
      if (n > 0) r_.notes.push_back(std::to_string(n) + " " + what);
    };
    count_note(issues_without_id_, "issues without an id were skipped");
    if (put_failed_ > 0) {
      r_.notes.push_back(std::to_string(put_failed_) + " records were not stored (first: " +
                         first_put_error_ + ")");
    }
    count_note(raw_failed_, "raw replies could not be kept, so their records have no raw_ref");
    count_note(raw_over_budget_, "raw replies were not kept: the per-sync raw evidence budget was reached");
    count_note(events_missing_, "issues had no latest event in Sentry (HTTP 404)");
    if (events_failed_ > 0) {
      r_.notes.push_back(std::to_string(events_failed_) + " latest events could not be read (first: " +
                         first_event_error_ + "); those issues have no release or stack");
    }
    if (events_not_fetched_ > 0) {
      r_.notes.push_back("latest events were read for " + std::to_string(events_fetched_) +
                         " issues only; " + std::to_string(events_not_fetched_) +
                         " issues have no release or stack (max_latest_events)");
    }
    r_.cursor.extra = cursor_.extra.is_object() ? cursor_.extra : json::Value::object();
    const bool sweep = full_sweep();
    if (sweep) {
      r_.notes.push_back("read every issue seen in the last " + std::to_string(client_.settings().lookback_days) +
                         " days (done once a day), so resolved and ignored issues are current too");
    }
    r_.cursor.extra.set("sweeping", json::Value::boolean(sweep && (stopped_ || incomplete_)));
    if (sweep && !stopped_ && !incomplete_) r_.cursor.extra.set("last_full_at", json::Value::string(ctx_.now_iso));
    if (stopped_) {
      // Records already written are kept and the watermark stays where it
      // was: the cache never looks more current than it is.
      r_.status = r_.records_written > 0 ? SyncStatus::kPartial : SyncStatus::kFailed;
    } else if (incomplete_) {
      r_.status = SyncStatus::kPartial;
    } else {
      r_.status = SyncStatus::kComplete;
      r_.cursor.resume.clear();
      // Only a complete sync moves the watermark, and only forward.
      const auto prev = epoch(cursor_.watermark);
      if (max_last_seen_t_ && (!prev || *max_last_seen_t_ > *prev)) r_.cursor.watermark = max_last_seen_;
    }
    return r_;
  }

  const ConnectorConfig& config_;
  const SyncCursor& cursor_;
  signals::SignalSink& sink_;
  const ConnectorContext& ctx_;
  const Client& client_;
  SyncResult r_;

  std::map<std::string, std::string> commits_;  // release version -> commit sha
  bool stopped_ = false;
  bool incomplete_ = false;
  std::size_t raw_bytes_ = 0;
  int raw_failed_ = 0, raw_over_budget_ = 0;
  int put_failed_ = 0;
  std::string first_put_error_;
  int issues_without_id_ = 0;
  int events_fetched_ = 0, events_missing_ = 0, events_failed_ = 0, events_not_fetched_ = 0;
  bool events_stopped_ = false;
  std::string first_event_error_;
  std::optional<std::int64_t> max_last_seen_t_;
  std::string max_last_seen_;
};

// ---- the connector ----

class SentryConnector : public signals::SignalConnector {
 public:
  signals::ConnectorInfo info() const override {
    signals::ConnectorInfo i;
    i.provider = kProvider;
    i.display_name = "Sentry";
    i.category = "production";
    i.egress = "Authenticated API requests (no local sessions, source or files) to the "
               "configured Sentry host";
    i.credential_env = "SENTRY_AUTH_TOKEN";
    i.credential_service = "com.devx.sentry";
    i.credential_help = "SENTRY_AUTH_TOKEN, or the Keychain item com.devx.sentry (an auth token "
                        "with org:read, project:read, event:read)";
    // A personal token: an organization token's fixed scopes are for CI and
    // do not read issues.
    i.credential_url = "{base_url}/settings/account/api/auth-tokens/";
    i.default_base_url = "https://sentry.io";
    i.settings = {
        {"base_url", "Sentry's address; default https://sentry.io. A self-hosted Sentry must use https"},
        {"organization", "The organization slug (required)"},
        {"project", "The project slug (required)"},
        {"environment", "Only issues seen in this environment (optional)"},
        {"lookback_days", "How far back the first sync reads issues; default 14"},
        {"max_issues", "The most issues one sync reads; default 200"},
        {"max_latest_events", "For how many of the most recently seen issues the latest event "
                              "(release, stack) is read; default 50"},
    };
    return i;
  }

  signals::ConnectorCapabilities capabilities() const override {
    signals::ConnectorCapabilities c;
    c.issues = true;
    c.crashes = true;
    c.deployments = true;
    c.incremental_pull = true;
    c.network = true;
    return c;
  }

  signals::ValidateResult validate(const ConnectorConfig& config, const ConnectorContext& ctx) override {
    signals::ValidateResult r;
    auto s = read_settings(config, true, &r.error);
    if (!s) return r;
    Client client(*s, ctx);
    if (!client.has_token()) {
      r.error = "no Sentry token: " + info().credential_help;
      return r;
    }
    const auto resp = client.get(client.api(client.org_path()));
    if (auto f = client.check(resp, "organization '" + s->org + "'")) {
      r.error = f->message;
      return r;
    }
    auto doc = parse_json(resp.body);
    if (!doc || !doc->is_object()) {
      r.error = "Sentry's reply for the organization was not a JSON object";
      return r;
    }
    r.ok = true;
    r.account = client.scrub(opt_str(*doc, "name").value_or(opt_str(*doc, "slug").value_or(s->org)));
    return r;
  }

  signals::DiscoverResult discover(const ConnectorConfig& config, const ConnectorContext& ctx) override {
    signals::DiscoverResult r;
    auto s = read_settings(config, false, &r.error);
    if (!s) return r;
    Client client(*s, ctx);
    if (!client.has_token()) {
      r.error = "no Sentry token: " + info().credential_help;
      return r;
    }
    std::string url = client.api(client.org_path() + "projects/");
    for (int page = 0; page < ctx.max_pages; page++) {
      if (ctx.cancel.cancelled()) {
        r.error = "cancelled after listing " + std::to_string(r.resources.size()) + " projects";
        return r;
      }
      const auto resp = client.get(url);
      if (auto f = client.check(resp, "the project list")) {
        // What was listed is still returned, but not as a complete list.
        r.error = f->message;
        return r;
      }
      auto doc = parse_json(resp.body);
      if (!doc || !doc->is_array()) {
        r.error = "Sentry's project list was not a JSON array";
        return r;
      }
      for (const auto& p : doc->items()) {
        if (!p.is_object()) continue;
        const std::string* slug = str_field(p, "slug");
        if (slug == nullptr) continue;
        json::Value one = json::Value::object();
        one.set("id", json::Value::string(*slug));
        one.set("name", json::Value::string(opt_str(p, "name").value_or(*slug)));
        one.set("kind", json::Value::string("project"));
        one.set("platform", str_or_null(opt_str(p, "platform")));
        r.resources.push_back(std::move(one));
      }
      const auto link = resp.header("link");
      const auto next = link ? net::next_link(*link) : std::nullopt;
      if (!next) {
        r.ok = true;
        return r;
      }
      if (!net::url_allowed(*next, {s->host})) {
        r.error = "Sentry's next-page link pointed away from " + s->host + "; the list is incomplete";
        return r;
      }
      url = *next;
    }
    r.error = "stopped after " + std::to_string(ctx.max_pages) + " pages; the project list is incomplete";
    return r;
  }

  SyncResult sync(const ConnectorConfig& config, const SyncCursor& cursor, signals::SignalSink& sink,
                  const ConnectorContext& ctx) override {
    SyncResult r;
    r.started_at = ctx.now_iso;
    r.cursor = cursor;
    r.cursor.connector_id = config.id;
    auto s = read_settings(config, true, &r.error);
    if (!s) return r;
    Client client(*s, ctx);
    if (!client.has_token()) {
      r.error = "no Sentry token: " + info().credential_help;
      return r;
    }
    return SyncRun(config, cursor, sink, ctx, client).run();
  }
};

}  // namespace

signals::ReleaseIdentity parse_sentry_release(const std::string& release) {
  ReleaseIdentity id;
  if (release.empty()) return id;
  // `package@version+build`. The last '@' splits, so an npm-style scoped
  // package ("@acme/web@1.2.3") keeps its own; a leading '@' alone is no
  // package.
  const auto at = release.rfind('@');
  if (at != std::string::npos && at > 0 && at + 1 < release.size()) {
    id.bundle_or_package_id = release.substr(0, at);
    const std::string rest = release.substr(at + 1);
    const auto plus = rest.find('+');
    if (plus == std::string::npos) {
      id.version = rest;
    } else {
      if (plus > 0) id.version = rest.substr(0, plus);
      if (plus + 1 < rest.size()) id.build_number = rest.substr(plus + 1);
    }
  } else {
    // A plain string. Teams that release by commit use the full SHA; a short
    // hex string could as well be a version number, so only a full one is
    // read as a commit.
    const std::string l = lower(release);
    if ((l.size() == 40 || l.size() == 64) && signals::looks_like_sha(l)) {
      id.commit_sha = l;
    } else {
      id.version = release;
    }
  }
  id.source = FactSource::kProvider;
  return id;
}

std::unique_ptr<signals::SignalConnector> make_sentry_connector() {
  return std::make_unique<SentryConnector>();
}

}  // namespace mpi::intelligence
