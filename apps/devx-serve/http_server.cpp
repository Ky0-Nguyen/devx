#include "apps/devx-serve/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

#include "core/util/json.hpp"

namespace mpi::devx {
namespace {

// Requests are small (a UI's API calls); anything larger is refused rather
// than buffered, per the bounded-input rule in spec section 15.
constexpr std::size_t kMaxRequestBytes = 4ull * 1024ull * 1024ull;
constexpr std::size_t kMaxHeaderCount = 100;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Unknown";
  }
}

std::string make_token() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  static const char* hex = "0123456789abcdef";
  std::string t;
  t.reserve(32);
  for (int i = 0; i < 32; ++i) t.push_back(hex[dist(rd)]);
  return t;
}

// Constant-time-ish comparison. The token is a local secret, not a password,
// but comparing without an early exit costs nothing here.
bool token_matches(const std::string& expected, const std::string& given) {
  if (expected.size() != given.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(expected[i] ^ given[i]);
  }
  return diff == 0;
}

bool read_request(int fd, std::string& raw, std::string* error) {
  char buf[16384];
  std::size_t header_end = std::string::npos;
  for (;;) {
    if (raw.size() > kMaxRequestBytes) {
      *error = "request exceeds the maximum size";
      return false;
    }
    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
    }
    if (header_end != std::string::npos) {
      // Headers complete; read the declared body if there is one.
      const std::string headers = lower(raw.substr(0, header_end));
      const std::size_t cl = headers.find("content-length:");
      std::size_t want = 0;
      if (cl != std::string::npos) {
        const std::size_t eol = headers.find("\r\n", cl);
        const std::string value =
            trim(headers.substr(cl + 15, eol == std::string::npos
                                             ? std::string::npos
                                             : eol - cl - 15));
        if (!value.empty() &&
            value.find_first_not_of("0123456789") == std::string::npos) {
          want = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        }
      }
      if (want > kMaxRequestBytes) {
        *error = "declared body exceeds the maximum size";
        return false;
      }
      if (raw.size() >= header_end + 4 + want) return true;
    }

    struct pollfd pfd{fd, POLLIN, 0};
    const int prc = ::poll(&pfd, 1, 5000);
    if (prc <= 0) {
      *error = prc == 0 ? "request timed out" : "poll failed";
      return false;
    }
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      // A client that closed after sending complete headers is fine.
      return header_end != std::string::npos;
    }
    raw.append(buf, static_cast<std::size_t>(n));
  }
}

bool parse_request(const std::string& raw, Request& req, std::string* error) {
  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    *error = "no header terminator";
    return false;
  }
  std::istringstream ss(raw.substr(0, header_end));
  std::string line;
  if (!std::getline(ss, line)) {
    *error = "empty request";
    return false;
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();

  std::istringstream rl(line);
  std::string target, version;
  if (!(rl >> req.method >> target >> version)) {
    *error = "malformed request line";
    return false;
  }

  const std::size_t qmark = target.find('?');
  req.path = url_decode(qmark == std::string::npos ? target : target.substr(0, qmark));
  if (qmark != std::string::npos) {
    std::istringstream qs(target.substr(qmark + 1));
    std::string pair;
    while (std::getline(qs, pair, '&')) {
      const std::size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        req.query[url_decode(pair)] = "";
      } else {
        req.query[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
      }
    }
  }

  std::size_t header_count = 0;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (++header_count > kMaxHeaderCount) {
      *error = "too many headers";
      return false;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  req.body = raw.substr(header_end + 4);
  return true;
}

}  // namespace

std::string Request::param(const std::string& key, const std::string& fallback) const {
  auto it = query.find(key);
  return it == query.end() ? fallback : it->second;
}
bool Request::has_param(const std::string& key) const {
  return query.find(key) != query.end();
}

Response Response::json(std::string payload, int status) {
  Response r;
  r.status = status;
  r.content_type = "application/json; charset=utf-8";
  r.body = std::move(payload);
  return r;
}
Response Response::text(std::string payload, int status) {
  Response r;
  r.status = status;
  r.content_type = "text/plain; charset=utf-8";
  r.body = std::move(payload);
  return r;
}
Response Response::html(std::string payload) {
  Response r;
  r.content_type = "text/html; charset=utf-8";
  r.body = std::move(payload);
  return r;
}
Response Response::error(int status, const std::string& message) {
  mpi::json::Value v = mpi::json::Value::object();
  v.set("error", mpi::json::Value::string(message));
  v.set("status", mpi::json::Value::integer(status));
  return json(v.dump(), status);
}

std::string url_decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (in[i] != '%') {
      out.push_back(in[i]);
      continue;
    }
    // A malformed escape stays literal rather than being dropped: silently
    // swallowing it could turn one path into a different valid one.
    if (i + 2 >= in.size() || !std::isxdigit(static_cast<unsigned char>(in[i + 1])) ||
        !std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      out.push_back('%');
      continue;
    }
    const auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    out.push_back(static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2])));
    i += 2;
  }
  return out;
}

std::string html_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

HttpServer::HttpServer() : token_(make_token()) {}

HttpServer::~HttpServer() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

void HttpServer::route(const std::string& method, const std::string& path,
                       Handler h) {
  Route r;
  r.method = method;
  r.path = path;
  // A trailing '/' marks a prefix route -- except for "/" itself, which as a
  // prefix would match every path and turn the page handler into a catch-all
  // that answers 200 for routes that do not exist.
  r.prefix = path.size() > 1 && path.back() == '/';
  r.handler = std::move(h);
  routes_.push_back(std::move(r));
}

bool HttpServer::listen(std::uint16_t port, std::string* error) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  // Loopback only, with no way to configure otherwise.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = std::string("bind 127.0.0.1:") + std::to_string(port) + ": " +
             std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    *error = std::string("listen: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    port_ = ntohs(bound.sin_port);
  } else {
    port_ = port;
  }
  return true;
}

Response HttpServer::dispatch(const Request& req) const {
  bool path_matched = false;
  for (const auto& r : routes_) {
    const bool hit = r.prefix ? req.path.rfind(r.path, 0) == 0 : req.path == r.path;
    if (!hit) continue;
    path_matched = true;
    if (r.method != req.method) continue;
    return r.handler(req);
  }
  if (path_matched) {
    return Response::error(405, "method " + req.method + " not allowed for " +
                                    req.path);
  }
  return Response::error(404, "no route for " + req.path);
}

void HttpServer::handle_connection(int fd) const {
  std::string raw;
  std::string error;
  Response resp;
  if (!read_request(fd, raw, &error)) {
    resp = Response::error(400, error);
  } else {
    Request req;
    if (!parse_request(raw, req, &error)) {
      resp = Response::error(400, error);
    } else {
      // Every request must present the token. Spec section 14 forbids an
      // unauthenticated listener, and loopback alone is not authentication:
      // any local process could reach it.
      std::string given = req.param("token");
      if (given.empty()) {
        auto it = req.headers.find("x-devx-token");
        if (it != req.headers.end()) given = it->second;
      }
      if (!token_matches(token_, given)) {
        resp = Response::error(
            401, "missing or invalid token; open the URL DevX printed on start");
      } else {
        resp = dispatch(req);
      }
    }
  }

  std::ostringstream head;
  head << "HTTP/1.1 " << resp.status << " " << status_text(resp.status) << "\r\n"
       << "Content-Type: " << resp.content_type << "\r\n"
       << "Content-Length: " << resp.body.size() << "\r\n"
       // The UI is served from this origin only; nothing here should be
       // embeddable or reachable cross-origin.
       << "X-Content-Type-Options: nosniff\r\n"
       << "X-Frame-Options: DENY\r\n"
       << "Referrer-Policy: no-referrer\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n";
  for (const auto& h : resp.headers) {
    head << h.first << ": " << h.second << "\r\n";
  }
  head << "\r\n";

  const std::string header_text = head.str();
  auto write_all = [fd](const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
      const ssize_t n = ::write(fd, data + sent, len - sent);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) continue;
        return;
      }
      sent += static_cast<std::size_t>(n);
    }
  };
  write_all(header_text.data(), header_text.size());
  write_all(resp.body.data(), resp.body.size());
}

void HttpServer::serve(const CancellationToken& cancel) {
  while (!cancel.cancelled()) {
    struct pollfd pfd{listen_fd_, POLLIN, 0};
    const int prc = ::poll(&pfd, 1, 250);
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (prc == 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    handle_connection(fd);
    ::close(fd);
  }
}

}  // namespace mpi::devx
