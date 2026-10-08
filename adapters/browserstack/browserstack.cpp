#include "adapters/browserstack/browserstack.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "core/util/process.hpp"

namespace mpi::browserstack {
namespace {

const char* kApi = "https://api-cloud.browserstack.com/";

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

std::string encode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

bool https_url(const std::string& u) { return u.rfind("https://", 0) == 0; }

// Runs curl with the credentials in an owner-only netrc file that is removed
// as soon as curl returns. The netrc names BrowserStack's two hosts only, so
// a data link to another host never receives them. The last line of stdout is
// the HTTP status.
Reply curl(const Credentials& c, std::vector<std::string> args, std::chrono::seconds timeout) {
  Reply r;
  if (!c.valid()) {
    r.error = "no BrowserStack credentials: set BROWSERSTACK_USERNAME and "
              "BROWSERSTACK_ACCESS_KEY, or add them in DevX's Emulator > BrowserStack";
    return r;
  }
  std::error_code ec;
  std::string pattern = (std::filesystem::temp_directory_path(ec) / "devx-bs-XXXXXX").string();
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');
  const int fd = ::mkstemp(tmpl.data());  // mkstemp creates it 0600
  if (fd < 0) {
    r.error = "could not create a temporary credentials file";
    return r;
  }
  std::string netrc;
  for (const char* host : {"api-cloud.browserstack.com", "hub-cloud.browserstack.com",
                           "api.browserstack.com"}) {
    netrc += std::string("machine ") + host + " login " + c.username + " password " +
             c.access_key + "\n";
  }
  const ssize_t n = ::write(fd, netrc.data(), netrc.size());
  ::close(fd);
  std::vector<std::string> argv = {"/usr/bin/curl", "-q", "-sS", "--max-time",
                                   std::to_string(timeout.count()), "--netrc-file", tmpl.data(),
                                   "-w", "\n%{http_code}"};
  argv.insert(argv.end(), args.begin(), args.end());
  proc::Options po;
  po.timeout = timeout + std::chrono::seconds(10);
  const auto res = n == static_cast<ssize_t>(netrc.size()) ? proc::run(argv, po) : proc::Result{};
  ::unlink(tmpl.data());
  if (!res.spawned || res.timed_out) {
    r.error = res.spawned ? "BrowserStack did not answer in time" : "could not run curl";
    return r;
  }
  if (res.exit_code != 0) {
    r.error = "curl failed: " + trim(res.err);
    return r;
  }
  const auto nl = res.out.rfind('\n');
  const std::string body = nl == std::string::npos ? std::string() : res.out.substr(0, nl);
  r.http_status = std::atoi(res.out.substr(nl == std::string::npos ? 0 : nl + 1).c_str());
  json::ParseError perr;
  json::Limits limits;
  limits.max_depth = 256;
  auto doc = json::parse(body, limits, &perr);
  if (doc) {
    r.body = *doc;
  } else {
    r.text = body;
  }
  r.ok = r.http_status >= 200 && r.http_status < 300;
  if (!r.ok) {
    std::string detail;
    if (doc) {
      // WebDriver errors carry value.message; REST errors carry message/error.
      if (const auto* v = doc->find("value"); v != nullptr && v->is_object()) {
        if (const auto* m = v->find("message"); m != nullptr && m->is_string()) detail = m->as_string();
      }
      for (const char* k : {"message", "error"}) {
        if (const auto* m = doc->find(k); detail.empty() && m != nullptr && m->is_string()) {
          detail = m->as_string();
        }
      }
    } else {
      detail = body.substr(0, 300);
    }
    r.error = r.http_status == 401 ? "BrowserStack refused the credentials (HTTP 401)"
                                   : "BrowserStack answered HTTP " + std::to_string(r.http_status) +
                                         (detail.empty() ? "" : ": " + detail);
  }
  return r;
}

}  // namespace

std::optional<Credentials> find_credentials() {
  const char* u = std::getenv("BROWSERSTACK_USERNAME");
  const char* k = std::getenv("BROWSERSTACK_ACCESS_KEY");
  if (u != nullptr && k != nullptr && *u && *k) return Credentials{u, k, "environment"};
  // The account name comes from the item's attributes; the key from -w.
  proc::Options po;
  po.timeout = std::chrono::seconds(30);
  const auto attrs = proc::run({"/usr/bin/security", "find-generic-password", "-s", kKeychainService}, po);
  if (!attrs.ok()) return std::nullopt;
  std::string account;
  std::istringstream in(attrs.out);
  std::string line;
  while (std::getline(in, line)) {
    const auto at = line.find("\"acct\"<blob>=\"");
    if (at != std::string::npos) {
      const auto start = at + 14;
      account = line.substr(start, line.rfind('"') - start);
    }
  }
  const auto key = proc::run({"/usr/bin/security", "find-generic-password", "-s", kKeychainService,
                              "-a", account, "-w"},
                             po);
  if (!key.ok() || account.empty()) return std::nullopt;
  return Credentials{account, trim(key.out), "keychain"};
}

json::Value Reply::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  o.set("http_status", json::Value::integer(http_status));
  if (!error.empty()) o.set("error", json::Value::string(error));
  o.set("body", body);
  return o;
}

Reply get(const Credentials& c, const std::string& path) {
  if (path.find("..") != std::string::npos || path.find("://") != std::string::npos) {
    Reply r;
    r.error = "not an API path: " + path;
    return r;
  }
  return curl(c, {std::string(kApi) + path}, std::chrono::seconds(120));
}

Reply request(const Credentials& c, const std::string& method, const std::string& url,
              const std::string& body, std::chrono::seconds timeout) {
  if (!https_url(url)) {
    Reply r;
    r.error = "refusing a URL that is not https: " + url;
    return r;
  }
  if (method != "GET" && method != "POST" && method != "DELETE") {
    Reply r;
    r.error = "unsupported method " + method;
    return r;
  }
  std::vector<std::string> args = {"-X", method, url};
  if (!body.empty()) {
    args.insert(args.end(), {"-H", "Content-Type: application/json", "--data-binary", body});
  }
  return curl(c, args, timeout);
}

Reply upload(const Credentials& c, const std::string& product, const std::string& file) {
  if (product != "app-live" && product != "app-automate") {
    Reply r;
    r.error = "product is app-live or app-automate";
    return r;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    Reply r;
    r.error = "no file at " + file;
    return r;
  }
  return curl(c, {"-X", "POST", std::string(kApi) + product + "/upload", "-F", "file=@" + file},
              std::chrono::seconds(600));
}

std::string app_live_url(const std::string& os, const std::string& os_version,
                         const std::string& device, const std::string& app_url) {
  std::string hash = app_url;
  if (hash.rfind("bs://", 0) == 0) hash = hash.substr(5);
  return "https://app-live.browserstack.com/dashboard#os=" + encode(os) +
         "&os_version=" + encode(os_version) + "&device=" + encode(device) +
         "&app_hashed_id=" + encode(hash) + "&scale_to_fit=true&speed=1&start=true";
}

}  // namespace mpi::browserstack
