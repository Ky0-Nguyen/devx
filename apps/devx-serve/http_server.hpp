// A minimal HTTP/1.1 server for DevX, the desktop UI.
//
// Written here rather than pulled in, for the reason ADR-0002 gives: the
// licence inventory stays empty and the build stays offline. It is deliberately
// small -- it serves a local UI, and nothing about it is meant for exposure.
//
// Two security properties are structural rather than configurable, because
// spec section 14 forbids an unauthenticated network listener:
//
//   * The socket binds to 127.0.0.1 only. There is no option to bind elsewhere.
//   * Every request must carry a token generated at startup, either as
//     `?token=` or an `X-DevX-Token` header. The token is printed once, in the
//     URL the operator opens.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"

namespace mpi::devx {

struct Request {
  std::string method;
  std::string path;                             // no query string
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;   // keys lowercased
  std::string body;

  std::string param(const std::string& key, const std::string& fallback = {}) const;
  bool has_param(const std::string& key) const;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  // Extra headers, e.g. Content-Disposition for a download.
  std::vector<std::pair<std::string, std::string>> headers;

  static Response json(std::string payload, int status = 200);
  static Response text(std::string payload, int status = 200);
  static Response html(std::string payload);
  // A JSON error body, so the UI never has to parse prose.
  static Response error(int status, const std::string& message);
};

using Handler = std::function<Response(const Request&)>;

class HttpServer {
 public:
  HttpServer();
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Registers an exact-path handler. Longest registered prefix wins for paths
  // ending in '/'.
  void route(const std::string& method, const std::string& path, Handler h);

  // Binds 127.0.0.1 on `port`, or an ephemeral port when `port` is 0.
  // Returns false and sets `error` on failure.
  bool listen(std::uint16_t port, std::string* error);

  // The port actually bound, valid after listen() succeeds.
  std::uint16_t port() const { return port_; }
  // The token every request must present.
  const std::string& token() const { return token_; }

  // Serves until `cancel` is cancelled. Blocking.
  void serve(const CancellationToken& cancel);

 private:
  Response dispatch(const Request& req) const;
  void handle_connection(int fd) const;

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::string token_;
  struct Route {
    std::string method;
    std::string path;
    bool prefix = false;
    Handler handler;
  };
  std::vector<Route> routes_;
};

// Percent-decodes a URL component. Invalid escapes are left literal rather
// than dropped, so a malformed path cannot silently become a different path.
std::string url_decode(const std::string& in);

// Escapes text for HTML text content.
std::string html_escape(const std::string& in);

}  // namespace mpi::devx
