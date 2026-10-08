#include "core/net/https_fetch.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "core/util/process.hpp"

namespace mpi::net {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// A curl config file value: double-quoted, with \ and " escaped. A newline
// would end the directive and start another, so one is refused outright.
bool config_quote(const std::string& v, std::string* out) {
  if (v.find_first_of("\r\n") != std::string::npos) return false;
  std::string q = "\"";
  for (char c : v) {
    if (c == '\\' || c == '"') q.push_back('\\');
    q.push_back(c);
  }
  q.push_back('"');
  *out = q;
  return true;
}

// An owner-only temporary file holding `content`; empty path on failure.
std::string temp_file(const std::string& content) {
  std::error_code ec;
  std::string pattern = (std::filesystem::temp_directory_path(ec) / "devx-http-XXXXXX").string();
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');
  const int fd = ::mkstemp(tmpl.data());  // created 0600
  if (fd < 0) return {};
  std::size_t done = 0;
  while (done < content.size()) {
    const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmpl.data());
      return {};
    }
    done += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return tmpl.data();
}

// Parses curl's -D dump. Only the last block counts: a redirect or a
// `100 Continue` leaves earlier ones.
void parse_headers(const std::string& dump, HttpResponse* r) {
  std::istringstream in(dump);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.rfind("HTTP/", 0) == 0) {
      r->headers.clear();
      const auto sp = line.find(' ');
      if (sp != std::string::npos) r->status = std::atoi(line.c_str() + sp + 1);
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    auto it = r->headers.find(name);
    if (name == "link" && it != r->headers.end()) {
      it->second += ", " + value;
    } else {
      r->headers[name] = value;
    }
  }
}

}  // namespace

std::optional<std::string> HttpResponse::header(const std::string& name) const {
  const auto it = headers.find(lower(name));
  if (it == headers.end()) return std::nullopt;
  return it->second;
}

std::string url_host(const std::string& url) {
  if (url.rfind("https://", 0) != 0) return {};
  std::string rest = url.substr(8);
  const auto end = rest.find_first_of("/?#");
  std::string authority = end == std::string::npos ? rest : rest.substr(0, end);
  // user@host is refused rather than parsed: nothing here sends credentials
  // in a URL, and one that arrives with them is not to be followed.
  if (authority.find('@') != std::string::npos) return {};
  const auto colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    authority = authority.substr(0, colon);
  }
  return lower(authority);
}

bool url_allowed(const std::string& url, const std::vector<std::string>& allowed) {
  const std::string host = url_host(url);
  if (host.empty()) return false;
  for (const auto& a : allowed) {
    if (lower(a) == host) return true;
  }
  return false;
}

std::string url_encode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

std::optional<std::string> next_link(const std::string& link_header) {
  // <url>; rel="next"; results="true"; cursor="...", <url>; rel="previous" ...
  std::size_t pos = 0;
  while (pos < link_header.size()) {
    const auto lt = link_header.find('<', pos);
    if (lt == std::string::npos) break;
    const auto gt = link_header.find('>', lt);
    if (gt == std::string::npos) break;
    const std::string url = link_header.substr(lt + 1, gt - lt - 1);
    auto next_lt = link_header.find('<', gt);
    const std::string params = lower(link_header.substr(
        gt + 1, next_lt == std::string::npos ? std::string::npos : next_lt - gt - 1));
    const bool is_next = params.find("rel=\"next\"") != std::string::npos ||
                         params.find("rel=next") != std::string::npos;
    const bool empty = params.find("results=\"false\"") != std::string::npos;
    if (is_next && !empty) return url;
    if (next_lt == std::string::npos) break;
    pos = next_lt;
  }
  return std::nullopt;
}

std::optional<int> retry_after_seconds(const HttpResponse& r) {
  const auto v = r.header("retry-after");
  if (!v || v->empty()) return std::nullopt;
  if (!std::all_of(v->begin(), v->end(), [](unsigned char c) { return std::isdigit(c); })) {
    return std::nullopt;
  }
  return std::atoi(v->c_str());
}

HttpResponse https_fetch(const HttpRequest& req) {
  HttpResponse r;
  if (!url_allowed(req.url, req.allowed_hosts)) {
    r.error = "refusing a request to " + (url_host(req.url).empty() ? req.url : url_host(req.url)) +
              ": only https to the provider's own host is allowed";
    return r;
  }
  if (req.method != "GET" && req.method != "POST") {
    r.error = "unsupported method " + req.method;
    return r;
  }
  // Headers, auth among them, go into the config file.
  std::string config;
  for (const auto& [name, value] : req.headers) {
    std::string quoted;
    if (name.find_first_of(":\r\n") != std::string::npos || !config_quote(name + ": " + value, &quoted)) {
      r.error = "refusing a header with a line break in it";
      return r;
    }
    config += "header = " + quoted + "\n";
  }
  const std::string config_path = temp_file(config);
  if (config_path.empty()) {
    r.error = "could not create a temporary request file";
    return r;
  }
  const std::string headers_path = temp_file("");
  if (headers_path.empty()) {
    ::unlink(config_path.c_str());
    r.error = "could not create a temporary request file";
    return r;
  }

  // -q first: no ~/.curlrc. A user's `location`, `proxy` or `trace` there
  // would otherwise follow redirects with the token header, or write it out.
  std::vector<std::string> argv = {"/usr/bin/curl", "-q", "-sS", "--proto", "=https", "--max-time",
                                   std::to_string(req.timeout.count()), "-K", config_path,
                                   "-D", headers_path, "-X", req.method,
                                   "--max-filesize", std::to_string(req.max_body_bytes)};
  if (!req.body.empty()) argv.insert(argv.end(), {"--data-binary", req.body});
  // `--` so a URL can never be read as an option.
  argv.insert(argv.end(), {"--", req.url});
  proc::Options po;
  po.timeout = req.timeout + std::chrono::seconds(10);
  po.max_output_bytes = req.max_body_bytes + 1;
  po.cancel = req.cancel;
  const auto res = proc::run(argv, po);
  ::unlink(config_path.c_str());

  std::string dump;
  {
    std::error_code ec;
    const auto size = std::filesystem::file_size(headers_path, ec);
    if (!ec && size < 1024 * 1024) {
      FILE* f = std::fopen(headers_path.c_str(), "rb");
      if (f != nullptr) {
        dump.resize(size);
        dump.resize(std::fread(dump.data(), 1, size, f));
        std::fclose(f);
      }
    }
  }
  ::unlink(headers_path.c_str());

  if (!res.spawned) {
    r.error = "could not run curl: " + res.spawn_error;
    return r;
  }
  if (res.cancelled) {
    r.error = "cancelled";
    return r;
  }
  if (res.timed_out) {
    r.error = "no answer within " + std::to_string(req.timeout.count()) + " s";
    return r;
  }
  parse_headers(dump, &r);
  // curl exits 63 when --max-filesize is hit and the size was announced.
  if (res.exit_code == 63) {
    r.ok = r.status != 0;
    r.truncated = true;
    r.body = res.out;
    if (!r.ok) r.error = "the reply is larger than " + std::to_string(req.max_body_bytes) + " bytes";
    return r;
  }
  if (res.exit_code != 0) {
    r.error = "curl failed: " + trim(res.err);
    return r;
  }
  r.ok = true;
  r.body = res.out;
  if (r.body.size() >= req.max_body_bytes) {
    r.truncated = true;
    r.body.resize(req.max_body_bytes);
  }
  return r;
}

std::optional<Secret> find_secret(const std::string& env_var,
                                  const std::vector<std::string>& keychain_services) {
  if (!env_var.empty()) {
    const char* v = std::getenv(env_var.c_str());
    if (v != nullptr && *v != '\0') return Secret{trim(v), "environment"};
  }
  for (const auto& service : keychain_services) {
    if (service.empty() || !proc::is_safe_argument(service, true)) continue;
    proc::Options po;
    po.timeout = std::chrono::seconds(30);
    const auto key = proc::run({"/usr/bin/security", "find-generic-password", "-s", service, "-w"}, po);
    if (!key.ok()) continue;
    const std::string value = trim(key.out);
    if (!value.empty()) return Secret{value, "keychain"};
  }
  return std::nullopt;
}

bool secret_present(const std::string& env_var, const std::vector<std::string>& keychain_services) {
  if (!env_var.empty()) {
    const char* v = std::getenv(env_var.c_str());
    if (v != nullptr && *v != '\0') return true;
  }
  for (const auto& service : keychain_services) {
    if (service.empty() || !proc::is_safe_argument(service, true)) continue;
    proc::Options po;
    po.timeout = std::chrono::seconds(10);
    // No -w: the item's attributes, not its secret.
    if (proc::run({"/usr/bin/security", "find-generic-password", "-s", service}, po).ok()) return true;
  }
  return false;
}

}  // namespace mpi::net
