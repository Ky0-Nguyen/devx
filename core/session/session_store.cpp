#include "core/session/session_store.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

#include "core/report/report.hpp"
#include "core/util/time.hpp"

namespace mpi::session {
namespace {

bool make_dir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

bool make_dirs(const std::string& path) {
  std::string acc;
  std::size_t start = 0;
  if (!path.empty() && path.front() == '/') {
    acc = "/";
    start = 1;
  }
  std::size_t pos = start;
  while (pos <= path.size()) {
    const std::size_t slash = path.find('/', pos);
    const std::string seg = path.substr(pos, slash == std::string::npos
                                                 ? std::string::npos
                                                 : slash - pos);
    if (!seg.empty()) {
      if (!acc.empty() && acc.back() != '/') acc += "/";
      acc += seg;
      if (!make_dir(acc)) return false;
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  return true;
}

bool write_file(const std::string& path, const std::string& content,
                std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot open for writing: " + path;
    return false;
  }
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.flush();
  if (!f) {
    // A full disk shows up here (spec D05).
    if (error) *error = "write failed (disk full?): " + path;
    return false;
  }
  return true;
}

bool remove_recursive(const std::string& path, std::vector<std::string>* removed) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return true;  // already gone
  if (S_ISDIR(st.st_mode)) {
    DIR* d = ::opendir(path.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent* e = ::readdir(d)) {
      const std::string name(e->d_name);
      if (name == "." || name == "..") continue;
      ok = remove_recursive(path + "/" + name, removed) && ok;
    }
    ::closedir(d);
    if (::rmdir(path.c_str()) != 0) return false;
    if (removed) removed->push_back(path + "/");
    return ok;
  }
  if (::unlink(path.c_str()) != 0) return false;
  if (removed) removed->push_back(path);
  return true;
}

}  // namespace

const char* to_string(SessionState s) {
  switch (s) {
    case SessionState::kIdle: return "idle";
    case SessionState::kDiscovering: return "discovering";
    case SessionState::kPreflight: return "preflight";
    case SessionState::kReady: return "ready";
    case SessionState::kRecording: return "recording";
    case SessionState::kStopping: return "stopping";
    case SessionState::kProcessing: return "processing";
    case SessionState::kCompleted: return "completed";
    case SessionState::kCancelled: return "cancelled";
    case SessionState::kFailed: return "failed";
    case SessionState::kInterrupted: return "interrupted";
    case SessionState::kPartial: return "partial";
  }
  return "idle";
}

json::Value SessionManifest::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string(schema_version));
  v.set("session_id", json::Value::string(session_id));
  v.set("created_at", json::Value::string(created_at));
  v.set("finalized_at", finalized_at.empty() ? json::Value::null()
                                             : json::Value::string(finalized_at));
  v.set("state", json::Value::string(to_string(state)));
  v.set("tool_version", json::Value::string(tool_version));
  v.set("requested_measurement_mode",
        json::Value::string(model::to_string(requested_mode)));
  json::Value fs = json::Value::array();
  for (const auto& f : files) fs.push_back(json::Value::string(f));
  v.set("files", std::move(fs));
  json::Value tr = json::Value::array();
  for (const auto& t : state_transitions) tr.push_back(json::Value::string(t));
  v.set("state_transitions", std::move(tr));
  v.set("synthetic", json::Value::boolean(synthetic));
  json::Value pr = json::Value::array();
  for (const auto& p : partial_reasons) pr.push_back(json::Value::string(p));
  v.set("partial_reasons", std::move(pr));
  v.set("checksum_algorithm", json::Value::string("fnv1a64"));
  v.set("checksum_purpose",
        json::Value::string("corruption detection only; not a cryptographic "
                            "integrity guarantee"));
  return v;
}

std::optional<std::string> file_checksum(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::uint64_t h = 1469598103934665603ull;
  char buf[65536];
  while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
    const std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
      h ^= static_cast<unsigned char>(buf[i]);
      h *= 1099511628211ull;
    }
    if (n < static_cast<std::streamsize>(sizeof(buf))) break;
  }
  char out[24];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

std::string new_session_id() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  static const char* hex = "0123456789abcdef";
  std::string id = "s-";
  // Date prefix makes sessions sort chronologically on disk.
  const std::string now = time_util::now_iso8601_utc();
  for (const char c : now) {
    if (c >= '0' && c <= '9') id.push_back(c);
  }
  id.push_back('-');
  for (int i = 0; i < 8; ++i) id.push_back(hex[dist(rd)]);
  return id;
}

WriteResult write_package(const std::string& parent_dir,
                          const SessionManifest& manifest_in,
                          const model::NormalizedTrace& trace,
                          const model::AnalysisResult& analysis,
                          const model::DiscoverySnapshot& discovery,
                          const std::string& report_markdown,
                          const std::string& report_json) {
  WriteResult res;
  if (manifest_in.session_id.empty()) {
    res.error = "refusing to write a package with an empty session id";
    return res;
  }
  const std::string final_dir = parent_dir + "/" + manifest_in.session_id;
  // Written here, renamed into place only once everything is on disk.
  const std::string temp_dir = final_dir + ".partial";

  if (!make_dirs(temp_dir + "/raw")) {
    res.error = "cannot create session directory: " + temp_dir;
    return res;
  }

  SessionManifest manifest = manifest_in;
  manifest.files.clear();

  struct Entry {
    std::string rel;
    std::string content;
  };
  std::vector<Entry> entries;
  entries.push_back({"capabilities.json", trace.capabilities.to_json().dump(2) + "\n"});
  entries.push_back({"discovery.json", discovery.to_json().dump(2) + "\n"});
  entries.push_back({"build.json", trace.build.to_json().dump(2) + "\n"});

  {
    // capture-config.json records what was asked for, separately from what was
    // achieved, so a later reader can tell the difference.
    json::Value cfg = json::Value::object();
    cfg.set("schema_version", json::Value::string("2.0"));
    cfg.set("requested_measurement_mode",
            json::Value::string(model::to_string(trace.requested_mode)));
    cfg.set("target", trace.target.to_json());
    cfg.set("device", trace.device.to_json());
    cfg.set("primary_clock_domain", json::Value::string(trace.primary_clock_domain));
    json::Value doms = json::Value::array();
    for (const auto& d : trace.clock_domains) doms.push_back(d.to_json());
    cfg.set("clock_domains", std::move(doms));
    entries.push_back({"capture-config.json", cfg.dump(2) + "\n"});
  }

  // Raw trace: written once and never rewritten (spec section 14: immutable
  // raw traces).
  entries.push_back({"raw/trace.mpi.json", trace.to_json(true).dump(2) + "\n"});
  entries.push_back({"issues.json", analysis.to_json().dump(2) + "\n"});
  entries.push_back({"report.md", report_markdown});
  entries.push_back({"report.json", report_json});

  for (const auto& e : entries) {
    std::string err;
    if (!write_file(temp_dir + "/" + e.rel, e.content, &err)) {
      res.error = err;
      remove_recursive(temp_dir, nullptr);
      return res;
    }
    manifest.files.push_back(e.rel);
  }

  // Checksums over everything written above.
  json::Value sums = json::Value::object();
  json::Value files_obj = json::Value::object();
  for (const auto& e : entries) {
    const auto sum = file_checksum(temp_dir + "/" + e.rel);
    files_obj.set(e.rel, sum.has_value() ? json::Value::string(*sum)
                                         : json::Value::null());
  }
  sums.set("schema_version", json::Value::string("2.0"));
  sums.set("algorithm", json::Value::string("fnv1a64"));
  sums.set("files", std::move(files_obj));
  std::string err;
  if (!write_file(temp_dir + "/checksums.json", sums.dump(2) + "\n", &err)) {
    res.error = err;
    remove_recursive(temp_dir, nullptr);
    return res;
  }
  manifest.files.push_back("checksums.json");

  if (manifest.finalized_at.empty()) {
    manifest.finalized_at = time_util::now_iso8601_utc();
  }
  if (!write_file(temp_dir + "/manifest.json", manifest.to_json().dump(2) + "\n",
                  &err)) {
    res.error = err;
    remove_recursive(temp_dir, nullptr);
    return res;
  }

  // Atomic finalization: the package becomes visible under its real name only
  // when it is complete.
  struct stat st{};
  if (::stat(final_dir.c_str(), &st) == 0) {
    res.error = "session directory already exists: " + final_dir;
    remove_recursive(temp_dir, nullptr);
    return res;
  }
  if (::rename(temp_dir.c_str(), final_dir.c_str()) != 0) {
    res.error = std::string("rename failed: ") + std::strerror(errno);
    remove_recursive(temp_dir, nullptr);
    return res;
  }

  res.ok = true;
  res.package_dir = final_dir;
  return res;
}

LoadResult load_package(const std::string& package_dir) {
  LoadResult res;
  const std::string manifest_path = package_dir + "/manifest.json";
  json::ParseError perr;
  auto parsed = json::parse_file(manifest_path, json::Limits{}, &perr);
  if (!parsed) {
    // A `.partial` sibling is the recoverable case worth naming explicitly.
    struct stat st{};
    if (::stat((package_dir + ".partial").c_str(), &st) == 0) {
      res.error =
          "no finalized manifest, but an interrupted write exists at " +
          package_dir + ".partial -- the session did not complete";
    } else {
      res.error = "cannot read manifest: " + perr.message;
    }
    return res;
  }
  const json::Value& m = *parsed;
  auto str = [&](const char* k) {
    const json::Value* v = m.find(k);
    return v && v->is_string() ? v->as_string() : std::string();
  };
  res.manifest.schema_version = str("schema_version");
  res.manifest.session_id = str("session_id");
  res.manifest.created_at = str("created_at");
  res.manifest.finalized_at = str("finalized_at");
  res.manifest.tool_version = str("tool_version");
  res.manifest.requested_mode =
      model::measurement_mode_from_string(str("requested_measurement_mode"));
  const json::Value* syn = m.find("synthetic");
  res.manifest.synthetic = syn && syn->as_bool();
  const std::string state = str("state");
  for (int i = 0; i <= static_cast<int>(SessionState::kPartial); ++i) {
    if (state == to_string(static_cast<SessionState>(i))) {
      res.manifest.state = static_cast<SessionState>(i);
      break;
    }
  }
  if (const json::Value* fs = m.find("files"); fs && fs->is_array()) {
    for (const auto& f : fs->items()) {
      if (f.is_string()) res.manifest.files.push_back(f.as_string());
    }
  }
  if (const json::Value* pr = m.find("partial_reasons"); pr && pr->is_array()) {
    for (const auto& p : pr->items()) {
      if (p.is_string()) res.manifest.partial_reasons.push_back(p.as_string());
    }
  }

  // Verify checksums. A mismatch is reported; the caller decides.
  auto sums = json::parse_file(package_dir + "/checksums.json", json::Limits{}, &perr);
  if (sums) {
    if (const json::Value* files = sums->find("files"); files && files->is_object()) {
      for (const auto& kv : files->members()) {
        if (!kv.second.is_string()) continue;
        const auto actual = file_checksum(package_dir + "/" + kv.first);
        if (!actual.has_value()) {
          res.checksum_failures.push_back(kv.first + ": file missing");
        } else if (*actual != kv.second.as_string()) {
          res.checksum_failures.push_back(kv.first + ": checksum mismatch (expected " +
                                          kv.second.as_string() + ", got " + *actual +
                                          ")");
        }
      }
    }
  } else {
    res.checksum_failures.push_back("checksums.json missing or unreadable");
  }

  res.trace_path = package_dir + "/raw/trace.mpi.json";
  res.ok = true;
  return res;
}

DeleteResult delete_package(const std::string& package_dir,
                            const std::string& expected_session_id) {
  DeleteResult res;
  if (expected_session_id.empty()) {
    res.error = "refusing to delete without an expected session id";
    return res;
  }
  // The directory must prove it is the session we were told to delete.
  auto parsed = json::parse_file(package_dir + "/manifest.json", json::Limits{}, nullptr);
  if (!parsed) {
    res.error = package_dir +
                " has no readable manifest.json; refusing to delete a "
                "directory that is not a session package";
    return res;
  }
  const json::Value* id = parsed->find("session_id");
  if (!id || !id->is_string() || id->as_string() != expected_session_id) {
    res.error = package_dir + " belongs to session '" +
                (id && id->is_string() ? id->as_string() : std::string("unknown")) +
                "', not '" + expected_session_id + "'; nothing was deleted";
    return res;
  }
  if (!remove_recursive(package_dir, &res.removed)) {
    res.error = "deletion incomplete; some files remain";
    return res;
  }
  res.ok = true;
  return res;
}

}  // namespace mpi::session
