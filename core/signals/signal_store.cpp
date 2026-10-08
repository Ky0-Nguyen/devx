#include "core/signals/signal_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "core/util/time.hpp"

namespace mpi::signals {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxRecordBytes = 4ull * 1024ull * 1024ull;
constexpr std::size_t kMaxRawBytes = 64ull * 1024ull * 1024ull;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool mkdirs(const std::string& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) return false;
  // Every directory from the intelligence root down is owner-only, not just
  // the leaf: raw/<provider> and signals/ hold the same evidence.
  const std::string marker = "/intelligence";
  const auto root = path.find(marker);
  if (root == std::string::npos) {
    ::chmod(path.c_str(), 0700);
    return true;
  }
  for (std::size_t at = root + marker.size(); at != std::string::npos;
       at = path.find('/', at + 1)) {
    ::chmod(path.substr(0, at).c_str(), 0700);
  }
  ::chmod(path.c_str(), 0700);
  return true;
}

bool write_atomic(const std::string& path, const std::string& bytes, std::string* error) {
  const std::string dir = fs::path(path).parent_path().string();
  if (!mkdirs(dir)) {
    if (error != nullptr) *error = "cannot create " + dir;
    return false;
  }
  std::random_device rd;
  const std::string tmp = path + ".tmp" + std::to_string(rd() % 100000);
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    if (error != nullptr) *error = "cannot write " + tmp + ": " + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      if (error != nullptr) *error = "write failed: " + tmp;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (error != nullptr) *error = "cannot move " + tmp + " into place";
    return false;
  }
  return true;
}

std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes,
                                     std::string* error) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) {
    if (error != nullptr) *error = "cannot read " + path;
    return std::nullopt;
  }
  if (size > max_bytes) {
    if (error != nullptr) *error = path + " is larger than " + std::to_string(max_bytes) + " bytes";
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  std::string s(static_cast<std::size_t>(size), '\0');
  in.read(s.data(), static_cast<std::streamsize>(size));
  if (!in && size > 0) {
    if (error != nullptr) *error = "cannot read " + path;
    return std::nullopt;
  }
  return s;
}

std::optional<json::Value> read_json(const std::string& path, std::string* error) {
  auto text = read_file(path, kMaxRecordBytes, error);
  if (!text) return std::nullopt;
  json::ParseError perr;
  auto v = json::parse(*text, json::Limits{}, &perr);
  if (!v && error != nullptr) *error = path + ": not valid JSON";
  return v;
}

bool write_json(const std::string& path, const json::Value& v, std::string* error) {
  return write_atomic(path, v.dump(1) + "\n", error);
}

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A raw name part: provider, category or file name.
std::string safe_part(const std::string& s, bool allow_dot) {
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || (allow_dot && c == '.');
    out.push_back(ok ? c : '_');
    if (out.size() >= 120) break;
  }
  // Never "." or "..", never hidden.
  while (!out.empty() && out[0] == '.') out[0] = '_';
  return out;
}

std::string date_dir(const SignalRecord& r) {
  for (const std::string* t : {&r.occurred_at, &r.observed_at}) {
    if (const auto s = parse_iso8601(*t)) return format_iso8601(*s).substr(0, 10);
  }
  return "undated";
}

json::Value index_entry(const SignalRecord& r, const std::string& rel_path) {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(r.id));
  o.set("path", json::Value::string(rel_path));
  o.set("provider", json::Value::string(r.provider));
  o.set("connector_id", json::Value::string(r.connector_id));
  o.set("kind", json::Value::string(r.kind));
  o.set("external_id", json::Value::string(r.external_id));
  if (r.severity) o.set("severity", json::Value::string(*r.severity));
  if (r.environment) o.set("environment", json::Value::string(*r.environment));
  if (r.title) o.set("title", json::Value::string(r.title->substr(0, 200)));
  o.set("occurred_at", json::Value::string(r.occurred_at));
  o.set("observed_at", json::Value::string(r.observed_at));
  const std::string key = r.release.key();
  if (!key.empty()) o.set("release_key", json::Value::string(key));
  if (r.release.version) o.set("version", json::Value::string(*r.release.version));
  if (r.release.build_number) o.set("build_number", json::Value::string(*r.release.build_number));
  if (r.release.commit_sha) o.set("commit_sha", json::Value::string(*r.release.commit_sha));
  o.set("basis", json::Value::string(to_string(r.basis)));
  if (!r.raw_ref.empty()) o.set("raw_ref", json::Value::string(r.raw_ref));
  return o;
}

std::string jstr(const json::Value& o, const char* k) {
  const json::Value* v = o.find(k);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

// Every raw ref a record points at: its own, and any attribute named
// *raw_ref or *raw_refs (Sentry's event, Firebase's other export files).
void collect_raw_refs(const SignalRecord& r, std::set<std::string>* out) {
  if (!r.raw_ref.empty()) out->insert(r.raw_ref);
  for (const auto& [k, v] : r.attributes.members()) {
    if (ends_with(k, "raw_ref") && v.is_string() && !v.as_string().empty()) out->insert(v.as_string());
    if (ends_with(k, "raw_refs") && v.is_array()) {
      for (const auto& x : v.items()) {
        if (x.is_string() && !x.as_string().empty()) out->insert(x.as_string());
      }
    }
  }
}

// "name.v3.json" -> ("name.json", 3); "name.json" -> ("name.json", 1).
std::pair<std::string, int> raw_version(const std::string& file) {
  const auto dot = file.rfind('.');
  std::string stem = dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : file.substr(dot);
  const auto v = stem.rfind(".v");
  if (v != std::string::npos && v + 2 < stem.size() &&
      std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(v) + 2, stem.end(),
                  [](char c) { return c >= '0' && c <= '9'; })) {
    return {stem.substr(0, v) + ext, std::atoi(stem.c_str() + v + 2)};
  }
  // A file whose real extension is .vN would have no ext: same rule.
  if (ext.size() > 2 && ext[1] == 'v' &&
      std::all_of(ext.begin() + 2, ext.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return {stem, std::atoi(ext.c_str() + 2)};
  }
  return {file, 1};
}

std::uint64_t dir_bytes(const std::string& dir, int* files = nullptr) {
  std::uint64_t total = 0;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      total += it->file_size(ec);
      if (files != nullptr) (*files)++;
    }
  }
  return total;
}

}  // namespace

// ---- query ----

SignalQuery SignalQuery::from_json(const json::Value& v) {
  SignalQuery q;
  q.provider = jstr(v, "provider");
  q.connector_id = jstr(v, "connector_id");
  q.kind = jstr(v, "kind");
  q.severity = jstr(v, "severity");
  q.environment = jstr(v, "environment");
  q.release_key = jstr(v, "release_key");
  q.version = jstr(v, "version");
  q.commit = lower(jstr(v, "commit"));
  q.since = jstr(v, "since");
  q.until = jstr(v, "until");
  q.text = jstr(v, "text");
  if (const json::Value* l = v.find("limit"); l != nullptr && l->is_number()) {
    q.limit = static_cast<std::size_t>(std::clamp<std::int64_t>(l->as_int(), 1, 5000));
  }
  return q;
}

json::Value SignalQueryResult::to_json() const {
  json::Value o = json::Value::object();
  json::Value a = json::Value::array();
  for (const auto& e : entries) a.push_back(e);
  o.set("signals", std::move(a));
  o.set("total", json::Value::integer(static_cast<std::int64_t>(total)));
  o.set("returned", json::Value::integer(static_cast<std::int64_t>(entries.size())));
  json::Value p = json::Value::array();
  for (const auto& s : problems) p.push_back(json::Value::string(s));
  o.set("problems", std::move(p));
  return o;
}

json::Value RetentionReport::to_json() const {
  json::Value o = json::Value::object();
  o.set("applied", json::Value::boolean(applied));
  o.set("signals_expired", json::Value::integer(signals_expired));
  o.set("raw_deleted", json::Value::integer(raw_deleted));
  o.set("bytes_freed", json::Value::integer(static_cast<std::int64_t>(bytes_freed)));
  o.set("kept_by_pin", json::Value::integer(kept_by_pin));
  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  o.set("notes", std::move(n));
  return o;
}

std::string intelligence_root(const std::string& sessions_dir) {
  return sessions_dir + "/intelligence";
}

// ---- the sink ----

class StoreSink : public SignalSink {
 public:
  StoreSink(SignalStore* store, std::string ws) : store_(store), ws_(std::move(ws)) {}
  ~StoreSink() override {
    // Flush: the index matches the records again; then drop raw versions a
    // re-sync superseded and nothing points at any more.
    std::vector<std::string> problems;
    store_->rebuild_index(ws_, &problems);
    store_->prune_superseded_raw(ws_);
  }
  std::string put_raw(const std::string& provider, const std::string& category,
                      const std::string& name, const std::string& bytes) override {
    return store_->put_raw(ws_, provider, category, name, bytes);
  }
  bool put_signal(SignalRecord record, std::string* error) override {
    return store_->put_signal(ws_, std::move(record), error);
  }
  std::optional<SignalRecord> existing(const std::string& id) override {
    std::string err;
    return store_->signal(ws_, id, &err);
  }

 private:
  SignalStore* store_;
  std::string ws_;
};

// ---- the store ----

SignalStore::SignalStore(std::string sessions_dir) : sessions_dir_(std::move(sessions_dir)) {}

std::string SignalStore::workspace_dir(const std::string& ws) const {
  return intelligence_root(sessions_dir_) + "/" + ws;
}

std::vector<ProjectWorkspace> SignalStore::workspaces(std::vector<std::string>* problems) const {
  std::vector<ProjectWorkspace> out;
  for (const auto& name : list_dir(intelligence_root(sessions_dir_))) {
    if (!id_is_safe(name)) continue;
    std::string err;
    if (auto w = workspace(name, &err)) {
      out.push_back(std::move(*w));
    } else if (problems != nullptr) {
      problems->push_back(name + ": " + err);
    }
  }
  return out;
}

std::optional<ProjectWorkspace> SignalStore::workspace(const std::string& id,
                                                       std::string* error) const {
  if (!id_is_safe(id)) {
    if (error != nullptr) *error = "not a workspace id: '" + id + "'";
    return std::nullopt;
  }
  const std::string path = workspace_dir(id) + "/workspace.json";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (error != nullptr) *error = "no workspace '" + id + "'";
    return std::nullopt;
  }
  auto v = read_json(path, error);
  if (!v) return std::nullopt;
  return ProjectWorkspace::from_json(*v, error);
}

bool SignalStore::save_workspace(const ProjectWorkspace& w, std::string* error) {
  if (!id_is_safe(w.id)) {
    if (error != nullptr) *error = "a workspace id is [a-z0-9_-], up to 64 characters";
    return false;
  }
  if (sessions_dir_.empty()) {
    if (error != nullptr) *error = "no sessions directory";
    return false;
  }
  mkdirs(intelligence_root(sessions_dir_));
  return write_json(workspace_dir(w.id) + "/workspace.json", w.to_json(), error);
}

bool SignalStore::delete_workspace(const std::string& id, std::string* error) {
  if (!id_is_safe(id)) {
    if (error != nullptr) *error = "not a workspace id";
    return false;
  }
  std::error_code ec;
  fs::remove_all(workspace_dir(id), ec);
  if (ec && error != nullptr) *error = ec.message();
  return !ec;
}

std::vector<ConnectorConfig> SignalStore::connectors(const std::string& ws,
                                                     std::vector<std::string>* problems) const {
  std::vector<ConnectorConfig> out;
  if (!id_is_safe(ws)) return out;
  const std::string dir = workspace_dir(ws) + "/connectors";
  for (const auto& name : list_dir(dir)) {
    if (!ends_with(name, ".json")) continue;
    std::string err;
    auto v = read_json(dir + "/" + name, &err);
    std::optional<ConnectorConfig> c;
    if (v) c = ConnectorConfig::from_json(*v, &err);
    if (c) {
      out.push_back(std::move(*c));
    } else if (problems != nullptr) {
      problems->push_back(name + ": " + err);
    }
  }
  return out;
}

std::optional<ConnectorConfig> SignalStore::connector(const std::string& ws, const std::string& id,
                                                      std::string* error) const {
  if (!id_is_safe(ws) || !id_is_safe(id)) {
    if (error != nullptr) *error = "not a connector id";
    return std::nullopt;
  }
  const std::string path = workspace_dir(ws) + "/connectors/" + id + ".json";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (error != nullptr) *error = "no connector '" + id + "' in workspace '" + ws + "'";
    return std::nullopt;
  }
  auto v = read_json(path, error);
  if (!v) return std::nullopt;
  return ConnectorConfig::from_json(*v, error);
}

bool SignalStore::save_connector(const std::string& ws, const ConnectorConfig& c,
                                 std::string* error) {
  std::string err;
  if (!workspace(ws, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  // Round-trip through the validating reader: a secret-looking setting is
  // refused here, before anything reaches disk.
  if (!ConnectorConfig::from_json(c.to_json(), error)) return false;
  return write_json(workspace_dir(ws) + "/connectors/" + c.id + ".json", c.to_json(), error);
}

bool SignalStore::remove_connector(const std::string& ws, const std::string& id,
                                   bool delete_local_data, std::string* error) {
  auto c = connector(ws, id, error);
  if (!c) return false;
  const std::string dir = workspace_dir(ws);
  ::unlink((dir + "/connectors/" + id + ".json").c_str());
  ::unlink((dir + "/cursors/" + id + ".json").c_str());
  if (!delete_local_data) return true;
  // Its records, and raw evidence nothing else references.
  std::vector<std::string> problems;
  std::set<std::string> kept_raw;
  std::vector<std::string> removed_raw;
  for (const auto& r : all_signals(ws, &problems)) {
    if (r.connector_id == id) {
      for (const auto& date : list_dir(dir + "/signals")) {
        ::unlink((dir + "/signals/" + date + "/" + r.id + ".json").c_str());
      }
      if (!r.raw_ref.empty()) removed_raw.push_back(r.raw_ref);
    } else if (!r.raw_ref.empty()) {
      kept_raw.insert(r.raw_ref);
    }
  }
  for (const auto& ref : removed_raw) {
    if (!kept_raw.count(ref) && ref.rfind("raw/", 0) == 0 && ref.find("..") == std::string::npos) {
      ::unlink((dir + "/" + ref).c_str());
    }
  }
  return rebuild_index(ws, &problems);
}

SyncCursor SignalStore::cursor(const std::string& ws, const std::string& connector_id) const {
  SyncCursor c;
  c.connector_id = connector_id;
  if (!id_is_safe(ws) || !id_is_safe(connector_id)) return c;
  std::string err;
  if (auto v = read_json(workspace_dir(ws) + "/cursors/" + connector_id + ".json", &err)) {
    c = SyncCursor::from_json(*v);
    c.connector_id = connector_id;
  }
  return c;
}

bool SignalStore::save_cursor(const std::string& ws, const SyncCursor& c, std::string* error) {
  if (!id_is_safe(ws) || !id_is_safe(c.connector_id)) {
    if (error != nullptr) *error = "not a connector id";
    return false;
  }
  return write_json(workspace_dir(ws) + "/cursors/" + c.connector_id + ".json", c.to_json(), error);
}

json::Value SignalStore::sync_status(const std::string& ws) const {
  std::string err;
  if (!id_is_safe(ws)) return json::Value::object();
  auto v = read_json(workspace_dir(ws) + "/state/sync-status.json", &err);
  return v && v->is_object() ? *v : json::Value::object();
}

bool SignalStore::record_sync(const std::string& ws, const std::string& connector_id,
                              const SyncResult& r, std::string* error) {
  json::Value all = sync_status(ws);
  json::Value prev = json::Value::object();
  if (const json::Value* p = all.find(connector_id); p != nullptr && p->is_object()) prev = *p;
  json::Value s = json::Value::object();
  s.set("last_attempt_at", json::Value::string(r.finished_at));
  s.set("status", json::Value::string(to_string(r.status)));
  if (!r.error.empty()) s.set("error", json::Value::string(r.error));
  s.set("records_written", json::Value::integer(r.records_written));
  s.set("pages", json::Value::integer(r.pages));
  s.set("rate_limited", json::Value::boolean(r.rate_limited));
  if (r.retry_after_s) s.set("retry_after_s", json::Value::integer(*r.retry_after_s));
  json::Value notes = json::Value::array();
  for (const auto& n : r.notes) notes.push_back(json::Value::string(n));
  s.set("notes", std::move(notes));
  // The last success survives a failure after it: freshness is the newest
  // evidence known to be complete, not the newest attempt.
  if (r.status == SyncStatus::kComplete) {
    s.set("last_success_at", json::Value::string(r.finished_at));
  } else if (const json::Value* ls = prev.find("last_success_at")) {
    s.set("last_success_at", *ls);
  }
  all.set(connector_id, std::move(s));
  return write_json(workspace_dir(ws) + "/state/sync-status.json", all, error);
}

std::unique_ptr<SignalSink> SignalStore::open_sink(const std::string& ws,
                                                   const std::string& connector_id) {
  (void)connector_id;
  write_atomic(workspace_dir(ws) + "/state/index-dirty", "sync in progress\n", nullptr);
  return std::make_unique<StoreSink>(this, ws);
}

bool SignalStore::put_signal(const std::string& ws, SignalRecord r, std::string* error) {
  if (!id_is_safe(ws)) {
    if (error != nullptr) *error = "not a workspace id";
    return false;
  }
  r.workspace_id = ws;
  if (r.id.empty()) r.id = make_signal_id(r.connector_id, r.external_id);
  // Validate exactly as a reader will.
  std::string err;
  auto checked = SignalRecord::from_json(r.to_json(), &err);
  if (!checked) {
    if (error != nullptr) *error = err;
    return false;
  }
  const std::string dir = workspace_dir(ws) + "/signals";
  const std::string date = date_dir(r);
  // One file per id: an update that moved the record's date removes the old.
  for (const auto& d : list_dir(dir)) {
    if (d != date && d != "index.jsonl") ::unlink((dir + "/" + d + "/" + r.id + ".json").c_str());
  }
  write_atomic(workspace_dir(ws) + "/state/index-dirty", "records changed\n", nullptr);
  return write_json(dir + "/" + date + "/" + r.id + ".json", r.to_json(), error);
}

std::string SignalStore::put_raw(const std::string& ws, const std::string& provider,
                                 const std::string& category, const std::string& name,
                                 const std::string& bytes) {
  if (!id_is_safe(ws) || bytes.size() > kMaxRawBytes) return {};
  const std::string p = safe_part(provider, false), c = safe_part(category, false);
  std::string n = safe_part(name, true);
  if (p.empty() || c.empty() || n.empty()) return {};
  const std::string base_dir = workspace_dir(ws) + "/raw/" + p + "/" + c;
  // Same name, same bytes: same ref. Same name, new bytes: a new version, so
  // anything that cited the old evidence still finds it unchanged. Only the
  // newest version is compared: re-reading every old one made each sync
  // slower than the last.
  const auto dot = n.rfind('.');
  const std::string stem = dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : n.substr(dot);
  auto file_for = [&](int v) { return v == 1 ? n : stem + ".v" + std::to_string(v) + ext; };
  int newest = 0;
  for (const auto& f : list_dir(base_dir)) {
    const auto [base, v] = raw_version(f);
    if (base == n && v > newest) newest = v;
  }
  if (newest > 0) {
    std::string err;
    if (auto existing = read_file(base_dir + "/" + file_for(newest), kMaxRawBytes, &err);
        existing && *existing == bytes) {
      return "raw/" + p + "/" + c + "/" + file_for(newest);
    }
  }
  std::string err;
  if (!write_atomic(base_dir + "/" + file_for(newest + 1), bytes, &err)) return {};
  return "raw/" + p + "/" + c + "/" + file_for(newest + 1);
}

std::optional<SignalRecord> SignalStore::signal(const std::string& ws, const std::string& id,
                                                std::string* error) const {
  if (!id_is_safe(ws) || !id_is_safe(id)) {
    if (error != nullptr) *error = "not a signal id: '" + id + "'";
    return std::nullopt;
  }
  const std::string dir = workspace_dir(ws) + "/signals";
  for (const auto& d : list_dir(dir)) {
    const std::string path = dir + "/" + d + "/" + id + ".json";
    std::error_code ec;
    if (!fs::exists(path, ec)) continue;
    auto v = read_json(path, error);
    if (!v) return std::nullopt;
    return SignalRecord::from_json(*v, error);
  }
  if (error != nullptr) *error = "no signal '" + id + "' in workspace '" + ws + "'";
  return std::nullopt;
}

std::vector<SignalRecord> SignalStore::all_signals(const std::string& ws,
                                                   std::vector<std::string>* problems) const {
  std::vector<SignalRecord> out;
  if (!id_is_safe(ws)) return out;
  const std::string dir = workspace_dir(ws) + "/signals";
  for (const auto& d : list_dir(dir)) {
    const std::string sub = dir + "/" + d;
    std::error_code ec;
    if (!fs::is_directory(sub, ec)) continue;
    for (const auto& f : list_dir(sub)) {
      if (!ends_with(f, ".json")) continue;
      std::string err;
      auto v = read_json(sub + "/" + f, &err);
      std::optional<SignalRecord> r;
      if (v) r = SignalRecord::from_json(*v, &err);
      if (r) {
        out.push_back(std::move(*r));
        continue;
      }
      // Reported and moved aside, never silently dropped -- and never allowed
      // to take the rest of the workspace down with it.
      const std::string q = workspace_dir(ws) + "/quarantine";
      mkdirs(q);
      std::rename((sub + "/" + f).c_str(), (q + "/" + d + "-" + f).c_str());
      if (problems != nullptr) {
        problems->push_back("quarantined signals/" + d + "/" + f + ": " + err);
      }
    }
  }
  return out;
}

bool SignalStore::rebuild_index(const std::string& ws, std::vector<std::string>* problems) const {
  if (!id_is_safe(ws)) return false;
  std::string lines;
  for (const auto& r : all_signals(ws, problems)) {
    lines += index_entry(r, "signals/" + date_dir(r) + "/" + r.id + ".json").dump() + "\n";
  }
  std::string err;
  const bool ok = write_atomic(workspace_dir(ws) + "/signals/index.jsonl", lines, &err);
  if (ok) ::unlink((workspace_dir(ws) + "/state/index-dirty").c_str());
  if (!ok && problems != nullptr) problems->push_back(err);
  return ok;
}

SignalQueryResult SignalStore::query(const std::string& ws, const SignalQuery& q) const {
  SignalQueryResult out;
  if (!id_is_safe(ws)) {
    out.problems.push_back("not a workspace id");
    return out;
  }
  const std::string dir = workspace_dir(ws);
  std::error_code ec;
  if (fs::exists(dir + "/state/index-dirty", ec) || !fs::exists(dir + "/signals/index.jsonl", ec)) {
    rebuild_index(ws, &out.problems);
  }
  std::string err;
  auto text = read_file(dir + "/signals/index.jsonl", 256ull * 1024ull * 1024ull, &err);
  if (!text) return out;
  const auto since = q.since.empty() ? std::nullopt : parse_iso8601(q.since);
  const auto until = q.until.empty() ? std::nullopt : parse_iso8601(q.until);
  const std::string needle = lower(q.text);
  std::vector<json::Value> matches;
  std::istringstream in(*text);
  std::string line;
  int bad = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    json::ParseError perr;
    auto e = json::parse(line, json::Limits{}, &perr);
    if (!e || !e->is_object()) {
      bad++;
      continue;
    }
    auto eq = [&](const std::string& want, const char* key) {
      return want.empty() || jstr(*e, key) == want;
    };
    if (!eq(q.provider, "provider") || !eq(q.connector_id, "connector_id") || !eq(q.kind, "kind") ||
        !eq(q.severity, "severity") || !eq(q.environment, "environment") ||
        !eq(q.release_key, "release_key") || !eq(q.version, "version")) {
      continue;
    }
    if (!q.commit.empty() && jstr(*e, "commit_sha").rfind(q.commit, 0) != 0) continue;
    if (since || until) {
      const auto t = parse_iso8601(jstr(*e, "occurred_at"));
      // A record with no time is outside any time filter: unknown is not
      // "inside".
      if (!t || (since && *t < *since) || (until && *t > *until)) continue;
    }
    if (!needle.empty()) {
      const std::string hay =
          lower(jstr(*e, "title") + " " + jstr(*e, "external_id") + " " + jstr(*e, "kind"));
      if (hay.find(needle) == std::string::npos) continue;
    }
    matches.push_back(std::move(*e));
  }
  if (bad > 0) out.problems.push_back(std::to_string(bad) + " unreadable index line(s); run a rebuild");
  std::stable_sort(matches.begin(), matches.end(), [](const json::Value& a, const json::Value& b) {
    return jstr(a, "occurred_at") > jstr(b, "occurred_at");
  });
  out.total = matches.size();
  if (matches.size() > q.limit) matches.resize(q.limit);
  out.entries = std::move(matches);
  return out;
}

std::set<std::string> SignalStore::referenced_raw(const std::string& ws) const {
  std::set<std::string> out;
  std::vector<std::string> problems;
  for (const auto& r : all_signals(ws, &problems)) collect_raw_refs(r, &out);
  return out;
}

int SignalStore::prune_superseded_raw(const std::string& ws) {
  if (!id_is_safe(ws)) return 0;
  const std::string raw = workspace_dir(ws) + "/raw";
  std::error_code ec;
  if (!fs::exists(raw, ec)) return 0;
  const std::set<std::string> refs = referenced_raw(ws);
  int removed = 0;
  for (const auto& provider : list_dir(raw)) {
    for (const auto& category : list_dir(raw + "/" + provider)) {
      const std::string dir = raw + "/" + provider + "/" + category;
      std::map<std::string, std::vector<std::pair<int, std::string>>> by_base;
      for (const auto& f : list_dir(dir)) {
        const auto [base, v] = raw_version(f);
        by_base[base].emplace_back(v, f);
      }
      for (auto& [base, versions] : by_base) {
        if (versions.size() < 2) continue;
        std::sort(versions.begin(), versions.end());
        versions.pop_back();  // the newest stays
        for (const auto& [v, f] : versions) {
          const std::string ref = "raw/" + provider + "/" + category + "/" + f;
          if (refs.count(ref)) continue;  // cited (a pin, an older record): kept
          if (::unlink((dir + "/" + f).c_str()) == 0) removed++;
        }
      }
    }
  }
  return removed;
}

RawRead SignalStore::read_raw_lines(const std::string& ws, const std::string& raw_ref,
                                    std::size_t first, std::size_t last, std::size_t max_bytes) const {
  // The same checks as read_raw, by asking it for nothing.
  RawRead r = read_raw(ws, raw_ref, 0, 0);
  if (!r.ok) return r;
  std::ifstream in(workspace_dir(ws) + "/" + raw_ref, std::ios::binary);
  std::string line;
  std::size_t n = 0;
  r.bytes.clear();
  while (std::getline(in, line)) {
    n++;
    if (n < first) continue;
    if (n > last) break;
    if (r.bytes.size() + line.size() + 1 > max_bytes) {
      r.truncated = true;
      break;
    }
    r.bytes += line + "\n";
  }
  return r;
}

RawRead SignalStore::read_raw(const std::string& ws, const std::string& raw_ref,
                              std::uint64_t offset, std::size_t max_bytes) const {
  RawRead r;
  // raw/<provider>/<category>/<name>, each part as put_raw makes them.
  if (!id_is_safe(ws) || raw_ref.rfind("raw/", 0) != 0 || raw_ref.find("..") != std::string::npos ||
      raw_ref.find("//") != std::string::npos || raw_ref.find('\\') != std::string::npos) {
    r.error = "not a local raw evidence ref: '" + raw_ref + "'";
    return r;
  }
  for (char c : raw_ref) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '/';
    if (!ok) {
      r.error = "not a local raw evidence ref: '" + raw_ref + "'";
      return r;
    }
  }
  const std::string path = workspace_dir(ws) + "/" + raw_ref;
  std::error_code ec;
  r.size = fs::file_size(path, ec);
  if (ec) {
    r.error = "no raw evidence at " + raw_ref + " (removed by retention, or never stored)";
    return r;
  }
  std::ifstream in(path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(std::min<std::uint64_t>(offset, r.size)));
  const std::uint64_t left = r.size - std::min<std::uint64_t>(offset, r.size);
  const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, max_bytes));
  r.bytes.resize(n);
  in.read(r.bytes.data(), static_cast<std::streamsize>(n));
  r.bytes.resize(static_cast<std::size_t>(in.gcount()));
  r.truncated = offset + r.bytes.size() < r.size;
  r.ok = true;
  return r;
}

bool SignalStore::set_pin(const std::string& ws, const std::string& kind, const std::string& id,
                          bool pinned, std::string* error) {
  if (kind != "signal" && kind != "release") {
    if (error != nullptr) *error = "a pin is a signal or a release";
    return false;
  }
  if (id.empty() || id.size() > 200) {
    if (error != nullptr) *error = "nothing to pin";
    return false;
  }
  json::Value p = pins(ws);
  const std::string key = kind == "signal" ? "signals" : "releases";
  std::vector<std::string> ids;
  if (const json::Value* a = p.find(key); a != nullptr && a->is_array()) {
    for (const auto& x : a->items()) {
      if (x.is_string() && x.as_string() != id) ids.push_back(x.as_string());
    }
  }
  if (pinned) ids.push_back(id);
  json::Value a = json::Value::array();
  for (const auto& x : ids) a.push_back(json::Value::string(x));
  p.set(key, std::move(a));
  return write_json(workspace_dir(ws) + "/state/pins.json", p, error);
}

json::Value SignalStore::pins(const std::string& ws) const {
  json::Value p = json::Value::object();
  std::string err;
  if (id_is_safe(ws)) {
    if (auto v = read_json(workspace_dir(ws) + "/state/pins.json", &err); v && v->is_object()) p = *v;
  }
  for (const char* k : {"signals", "releases"}) {
    if (p.find(k) == nullptr) p.set(k, json::Value::array());
  }
  return p;
}

RetentionReport SignalStore::apply_retention(const std::string& ws, bool apply,
                                             std::int64_t now_epoch_s) {
  RetentionReport rep;
  rep.applied = apply;
  std::string err;
  auto w = workspace(ws, &err);
  if (!w) {
    rep.notes.push_back(err);
    return rep;
  }
  std::map<std::string, RetentionPolicy> by_connector;
  for (const auto& c : connectors(ws)) by_connector[c.id] = c.retention.value_or(w->retention);
  const json::Value p = pins(ws);
  std::set<std::string> pinned_signals, pinned_releases;
  for (const auto& [key, set] : {std::pair<const char*, std::set<std::string>*>{"signals", &pinned_signals},
                                 {"releases", &pinned_releases}}) {
    if (const json::Value* a = p.find(key); a != nullptr && a->is_array()) {
      for (const auto& x : a->items()) {
        if (x.is_string()) set->insert(x.as_string());
      }
    }
  }
  std::vector<std::string> problems;
  std::set<std::string> referenced_raw;
  const std::string dir = workspace_dir(ws);
  for (const auto& r : all_signals(ws, &problems)) {
    // A connector that was removed keeps its evidence under the workspace's
    // policy.
    const RetentionPolicy pol =
        by_connector.count(r.connector_id) ? by_connector[r.connector_id] : w->retention;
    // Age is time since DevX last stored the record: a provider still
    // reporting it refreshes it.
    const auto t = parse_iso8601(r.observed_at);
    const bool expired = pol.days > 0 && t && *t < now_epoch_s - std::int64_t{pol.days} * 86400;
    const bool pinned = pinned_signals.count(r.id) || pinned_releases.count(r.release.key());
    if (expired && pinned) rep.kept_by_pin++;
    if (!expired || pinned) {
      collect_raw_refs(r, &referenced_raw);
      continue;
    }
    rep.signals_expired++;
    if (apply) {
      const std::string path = dir + "/signals/" + date_dir(r) + "/" + r.id + ".json";
      std::error_code ec;
      rep.bytes_freed += fs::file_size(path, ec);
      ::unlink(path.c_str());
    }
  }
  // Raw evidence nothing retained points at, and that is itself older than
  // the shortest window in force: a file written by a sync that is still in
  // the window is not deleted just because no record names it yet. With
  // "forever" everywhere, no raw evidence is deleted at all.
  int shortest_days = w->retention.days;
  for (const auto& [_, pol] : by_connector) {
    if (pol.days > 0 && (shortest_days == 0 || pol.days < shortest_days)) shortest_days = pol.days;
  }
  std::error_code ec;
  if (shortest_days > 0 && fs::exists(dir + "/raw", ec)) {
    const std::int64_t horizon = now_epoch_s - std::int64_t{shortest_days} * 86400;
    for (auto it = fs::recursive_directory_iterator(dir + "/raw", ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string ref = "raw/" + fs::relative(it->path(), dir + "/raw", ec).string();
      if (referenced_raw.count(ref)) continue;
      struct stat sb {};
      if (::stat(it->path().c_str(), &sb) != 0 || static_cast<std::int64_t>(sb.st_mtime) > horizon) continue;
      rep.raw_deleted++;
      if (apply) {
        rep.bytes_freed += it->file_size(ec);
        fs::remove(it->path(), ec);
      }
    }
  }
  for (const auto& pr : problems) rep.notes.push_back(pr);
  if (apply) rebuild_index(ws, &rep.notes);
  return rep;
}

json::Value SignalStore::storage_usage(const std::string& ws) const {
  json::Value o = json::Value::object();
  if (!id_is_safe(ws)) return o;
  const std::string dir = workspace_dir(ws);
  int raw_files = 0, signal_files = 0;
  o.set("raw_bytes", json::Value::integer(static_cast<std::int64_t>(dir_bytes(dir + "/raw", &raw_files))));
  o.set("raw_files", json::Value::integer(raw_files));
  o.set("signal_bytes",
        json::Value::integer(static_cast<std::int64_t>(dir_bytes(dir + "/signals", &signal_files))));
  o.set("total_bytes", json::Value::integer(static_cast<std::int64_t>(dir_bytes(dir))));
  std::map<std::string, int> by_provider;
  std::string oldest;
  std::vector<std::string> problems;
  int count = 0;
  for (const auto& r : all_signals(ws, &problems)) {
    by_provider[r.provider]++;
    count++;
    if (!r.occurred_at.empty() && (oldest.empty() || r.occurred_at < oldest)) oldest = r.occurred_at;
  }
  json::Value bp = json::Value::object();
  for (const auto& [k, v] : by_provider) bp.set(k, json::Value::integer(v));
  o.set("signals", json::Value::integer(count));
  o.set("signals_by_provider", std::move(bp));
  if (!oldest.empty()) o.set("oldest_evidence_at", json::Value::string(oldest));
  int quarantined = 0;
  dir_bytes(dir + "/quarantine", &quarantined);
  o.set("quarantined", json::Value::integer(quarantined));
  return o;
}

}  // namespace mpi::signals
