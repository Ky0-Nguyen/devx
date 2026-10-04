#include "core/observe/observation_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "core/util/time.hpp"

namespace mpi::observe {
namespace {

constexpr const char* kSchema = "devx.observation/1";

std::string stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
  return buf;
}

std::string random_hex() {
  std::random_device rd;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(rd()));
  return buf;
}

bool kind_is_safe(const std::string& k) {
  if (k.empty() || k.size() > 32) return false;
  for (char c : k) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string observations_dir(const std::string& sessions_dir) {
  return sessions_dir + "/observations";
}

bool observation_id_is_safe(const std::string& id) {
  if (id.empty() || id.size() > 96) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_';
    if (!ok) return false;
  }
  return true;
}

SavedObservation save_observation(const std::string& sessions_dir,
                                  const std::string& kind,
                                  const std::string& app_identifier,
                                  const std::string& device_id,
                                  const json::Value& summary,
                                  const json::Value& document) {
  SavedObservation out;
  if (sessions_dir.empty()) {
    out.error = "no sessions directory to save into";
    return out;
  }
  if (!kind_is_safe(kind)) {
    out.error = "an observation kind is a short lowercase word";
    return out;
  }
  const std::string dir = observations_dir(sessions_dir);
  ::mkdir(sessions_dir.c_str(), 0700);
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    out.error = "cannot create " + dir;
    return out;
  }
  ::chmod(dir.c_str(), 0700);

  // Time first, so a plain sort of the names is newest-last across kinds.
  out.id = stamp() + "-" + kind + "-" + random_hex();
  out.path = dir + "/" + out.id + ".json";

  json::Value env = json::Value::object();
  env.set("schema", json::Value::string(kSchema));
  env.set("id", json::Value::string(out.id));
  env.set("kind", json::Value::string(kind));
  env.set("saved_at", json::Value::string(time_util::now_iso8601_utc()));
  env.set("app_identifier", json::Value::string(app_identifier));
  env.set("device_id", json::Value::string(device_id));
  env.set("summary", summary);
  env.set("document", document);

  const std::string tmp = out.path + ".tmp";
  {
    // Created owner-only before anything is written to it.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
      out.error = "cannot write " + tmp;
      return out;
    }
    const std::string body = env.dump(1) + "\n";
    std::size_t done = 0;
    while (done < body.size()) {
      const ssize_t n = ::write(fd, body.data() + done, body.size() - done);
      if (n <= 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        out.error = "write failed: " + tmp;
        return out;
      }
      done += static_cast<std::size_t>(n);
    }
    ::close(fd);
  }
  if (std::rename(tmp.c_str(), out.path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    out.error = "cannot move " + tmp + " into place";
    return out;
  }
  out.ok = true;
  return out;
}

json::Value list_observations(const std::string& sessions_dir,
                              const std::string& kind, std::size_t limit) {
  json::Value out = json::Value::object();
  json::Value arr = json::Value::array();
  const std::string dir = observations_dir(sessions_dir);
  std::vector<std::string> names;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (struct dirent* e = ::readdir(d)) {
      const std::string name(e->d_name);
      if (!ends_with(name, ".json")) continue;
      const std::string id = name.substr(0, name.size() - 5);
      if (!observation_id_is_safe(id)) continue;
      if (!kind.empty() && id.find("-" + kind + "-") == std::string::npos) continue;
      names.push_back(id);
    }
    ::closedir(d);
  }
  std::sort(names.rbegin(), names.rend());
  const std::size_t total = names.size();
  if (limit > 0 && names.size() > limit) names.resize(limit);
  for (const auto& id : names) {
    std::string err;
    auto doc = read_observation(sessions_dir, id, &err);
    json::Value e = json::Value::object();
    e.set("id", json::Value::string(id));
    if (!doc) {
      // Listed with its error, so a damaged file is visible rather than gone.
      e.set("error", json::Value::string(err));
    } else {
      for (const char* key : {"kind", "saved_at", "app_identifier", "device_id", "summary"}) {
        if (const json::Value* v = doc->find(key)) e.set(key, *v);
      }
    }
    arr.push_back(std::move(e));
  }
  out.set("directory", json::Value::string(dir));
  out.set("total", json::Value::integer(static_cast<std::int64_t>(total)));
  out.set("observations", std::move(arr));
  return out;
}

std::optional<json::Value> read_observation(const std::string& sessions_dir,
                                            const std::string& id,
                                            std::string* error) {
  if (!observation_id_is_safe(id)) {
    if (error != nullptr) *error = "not an observation id: '" + id + "'";
    return std::nullopt;
  }
  const std::string path = observations_dir(sessions_dir) + "/" + id + ".json";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error != nullptr) *error = "no observation with id '" + id + "'";
    return std::nullopt;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  json::Limits limits;
  limits.max_depth = 512;
  json::ParseError perr;
  auto doc = json::parse(ss.str(), limits, &perr);
  if (!doc) {
    if (error != nullptr) *error = "unreadable: " + perr.message;
    return std::nullopt;
  }
  return doc;
}

json::Value summarize_inspect(const json::Value& report) {
  json::Value s = json::Value::object();
  auto count = [&](const char* key) -> std::int64_t {
    const json::Value* v = report.find(key);
    return v != nullptr && v->is_array() ? static_cast<std::int64_t>(v->items().size()) : 0;
  };
  s.set("network", json::Value::integer(count("network")));
  s.set("console", json::Value::integer(count("console")));
  s.set("redux", json::Value::integer(count("redux")));
  s.set("screenshots", json::Value::integer(count("screenshots")));
  const json::Value* attached = report.find("debugger_attached");
  s.set("debugger_attached",
        json::Value::boolean(attached != nullptr && attached->is_bool() && attached->as_bool()));
  bool detail = false;
  if (const json::Value* net = report.find("network"); net != nullptr && net->is_array()) {
    for (const auto& e : net->items()) {
      for (const char* k : {"request_headers", "response_headers", "response_body",
                            "request_body"}) {
        const json::Value* v = e.find(k);
        if (v != nullptr && !v->is_null()) detail = true;
      }
    }
  }
  s.set("contains_headers_or_bodies", json::Value::boolean(detail));
  if (const json::Value* t = report.find("target_title"); t != nullptr && t->is_string()) {
    s.set("target_title", *t);
  }
  return s;
}

}  // namespace mpi::observe
