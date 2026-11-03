#include "core/update/update.hpp"

#include <cctype>
#include <vector>

namespace mpi::update {
namespace {

json::Value error_json(const std::string& why) {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(false));
  o.set("error", json::Value::string(why));
  return o;
}

std::string text(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

struct Parsed {
  std::vector<long> numbers;
  bool suffix = false;  // "-beta" and the like
};

Parsed parse_version(std::string v) {
  Parsed p;
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  std::size_t i = 0;
  while (i < v.size()) {
    if (!std::isdigit(static_cast<unsigned char>(v[i]))) {
      p.suffix = true;
      break;
    }
    long n = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
      n = n * 10 + (v[i] - '0');
      if (n > 1000000) n = 1000000;  // a bound, not a version anyone ships
      ++i;
    }
    p.numbers.push_back(n);
    if (i < v.size() && v[i] == '.') {
      ++i;
    } else if (i < v.size()) {
      p.suffix = true;
      break;
    }
  }
  return p;
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
  const Parsed pa = parse_version(a);
  const Parsed pb = parse_version(b);
  const std::size_t n = std::max(pa.numbers.size(), pb.numbers.size());
  for (std::size_t i = 0; i < n; ++i) {
    const long x = i < pa.numbers.size() ? pa.numbers[i] : 0;
    const long y = i < pb.numbers.size() ? pb.numbers[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  if (pa.suffix != pb.suffix) return pa.suffix ? -1 : 1;
  return 0;
}

json::Value parse_latest_release(const std::string& body, const std::string& current) {
  json::ParseError err;
  auto doc = json::parse(body, &err);
  if (!doc || !doc->is_object()) return error_json("GitHub's reply was not a release");
  const std::string tag = text(*doc, "tag_name");
  if (tag.empty()) return error_json("GitHub's reply names no release version");
  std::string latest = tag;
  if (latest[0] == 'v' || latest[0] == 'V') latest.erase(0, 1);

  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(true));
  o.set("current", json::Value::string(current));
  o.set("latest", json::Value::string(latest));
  o.set("newer", json::Value::boolean(compare_versions(latest, current) > 0));
  o.set("name", json::Value::string(text(*doc, "name")));
  o.set("notes", json::Value::string(text(*doc, "body")));
  o.set("published_at", json::Value::string(text(*doc, "published_at")));
  o.set("html_url", json::Value::string(text(*doc, "html_url")));

  // The release's own disk image, by the name the release workflow gives it,
  // and its checksum file. Anything else attached is ignored.
  const std::string dmg_name = "DevX-" + latest + ".dmg";
  std::string dmg_url, sha_url;
  if (const json::Value* assets = doc->find("assets"); assets != nullptr && assets->is_array()) {
    for (const auto& a : assets->items()) {
      const std::string name = text(a, "name");
      const std::string url = text(a, "browser_download_url");
      if (url.rfind("https://", 0) != 0) continue;
      if (name == dmg_name) dmg_url = url;
      if (name == dmg_name + ".sha256") sha_url = url;
    }
  }
  o.set("dmg_name", json::Value::string(dmg_name));
  o.set("dmg_url", json::Value::string(dmg_url));
  o.set("sha256_url", json::Value::string(sha_url));
  return o;
}

json::Value check(const std::string& current, const net::Transport& transport) {
  net::HttpRequest req;
  req.url = std::string("https://api.github.com/repos/") + kRepository + "/releases/latest";
  req.headers.emplace_back("Accept", "application/vnd.github+json");
  req.headers.emplace_back("User-Agent", "DevX/" + current);
  req.allowed_hosts = {"api.github.com"};
  req.timeout = std::chrono::seconds(20);
  req.max_body_bytes = 2u * 1024u * 1024u;
  const net::HttpResponse r = transport(req);
  if (!r.ok) return error_json("could not reach GitHub: " + r.error);
  if (r.status == 404) return error_json("GitHub lists no release of " + std::string(kRepository) + " yet");
  if (r.status == 403 || r.status == 429) {
    std::string why = "GitHub refused the request (HTTP " + std::to_string(r.status) + ")";
    if (auto left = r.header("x-ratelimit-remaining"); left && *left == "0") {
      why += ": its hourly limit for this network is used up, try again later";
    }
    return error_json(why);
  }
  if (!r.success()) return error_json("GitHub answered HTTP " + std::to_string(r.status));
  return parse_latest_release(r.body, current);
}

}  // namespace mpi::update
