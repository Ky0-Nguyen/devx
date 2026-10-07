#include "core/net/loopback_http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mpi::net {
namespace {

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace

GetResult loopback_get(std::uint16_t port, const std::string& path,
                       int timeout_ms, std::size_t max_bytes) {
  GetResult out;
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    out.error = std::string("socket: ") + std::strerror(errno);
    return out;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    out.error = "connect 127.0.0.1:" + std::to_string(port) + ": " +
                std::strerror(errno);
    ::close(fd);
    return out;
  }
  std::string req = "GET " + path + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                    "Accept: application/json\r\n"
                    // No keep-alive: one request, then the server closing is
                    // a perfectly good end-of-body signal.
                    "Connection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      out.error = std::string("send: ") + std::strerror(errno);
      ::close(fd);
      return out;
    }
    sent += static_cast<std::size_t>(n);
  }

  std::string buf;
  int remaining = timeout_ms;
  for (;;) {
    pollfd p{fd, POLLIN, 0};
    int slice = remaining > 200 ? 200 : remaining;
    if (slice <= 0) {
      out.error = "no response within " + std::to_string(timeout_ms) + "ms";
      ::close(fd);
      return out;
    }
    int rc = ::poll(&p, 1, slice);
    if (rc < 0) {
      if (errno == EINTR) continue;
      out.error = std::string("poll: ") + std::strerror(errno);
      ::close(fd);
      return out;
    }
    if (rc == 0) {
      remaining -= slice;
      continue;
    }
    char chunk[16384];
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      out.error = std::string("recv: ") + std::strerror(errno);
      ::close(fd);
      return out;
    }
    if (n == 0) break;   // the server closed: the body is complete
    buf.append(chunk, static_cast<std::size_t>(n));
    if (buf.size() > max_bytes) {
      out.error = "the response exceeded " + std::to_string(max_bytes) + " bytes";
      ::close(fd);
      return out;
    }
    std::size_t split = buf.find("\r\n\r\n");
    if (split == std::string::npos) continue;
    std::string head = lower(buf.substr(0, split));
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
      out.error = "the server used chunked encoding, which this client does "
                  "not decode";
      ::close(fd);
      return out;
    }
    std::size_t at = head.find("content-length:");
    if (at != std::string::npos) {
      std::size_t value = head.find_first_of("0123456789", at);
      if (value != std::string::npos) {
        std::size_t want = static_cast<std::size_t>(
            std::strtoull(head.c_str() + value, nullptr, 10));
        if (buf.size() >= split + 4 + want) break;
      }
    }
  }
  ::close(fd);

  std::size_t split = buf.find("\r\n\r\n");
  if (split == std::string::npos) {
    out.error = "the response had no header terminator";
    return out;
  }
  std::string status_line = buf.substr(0, buf.find("\r\n"));
  std::size_t sp = status_line.find(' ');
  if (sp != std::string::npos) {
    out.status = static_cast<int>(std::strtol(status_line.c_str() + sp + 1,
                                              nullptr, 10));
  }
  out.body = buf.substr(split + 4);
  out.ok = out.status >= 200 && out.status < 300;
  if (!out.ok && out.error.empty()) {
    out.error = "the server answered: " + status_line;
  }
  return out;
}

}  // namespace mpi::net
