#include "core/net/http2_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace mpi::net {
namespace {

enum : std::uint8_t {
  kData = 0,
  kHeaders = 1,
  kPriority = 2,
  kRstStream = 3,
  kSettings = 4,
  kPushPromise = 5,
  kPing = 6,
  kGoAway = 7,
  kWindowUpdate = 8,
  kContinuation = 9,
};
constexpr std::uint8_t kEndStream = 0x1;
constexpr std::uint8_t kAck = 0x1;
constexpr std::uint8_t kEndHeaders = 0x4;
constexpr std::uint8_t kPadded = 0x8;
constexpr std::uint8_t kPriorityFlag = 0x20;

constexpr std::uint32_t kMaxWindow = 0x7fffffff;
// What this side accepts per frame. A screen frame can be megabytes; larger
// frames mean fewer of them.
constexpr std::uint32_t kOurMaxFrame = 1u << 20;

void put32(std::string& s, std::uint32_t v) {
  s.push_back(static_cast<char>((v >> 24) & 0xff));
  s.push_back(static_cast<char>((v >> 16) & 0xff));
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

std::uint32_t get32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

bool read_exact(int fd, std::uint8_t* buf, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    got += static_cast<std::size_t>(r);
  }
  return true;
}

// Strips padding and priority from a HEADERS or DATA payload.
bool unpad(std::uint8_t type, std::uint8_t flags, std::string_view& payload) {
  std::size_t pad = 0;
  if (flags & kPadded) {
    if (payload.empty()) return false;
    pad = static_cast<std::uint8_t>(payload[0]);
    payload.remove_prefix(1);
  }
  if (type == kHeaders && (flags & kPriorityFlag)) {
    if (payload.size() < 5) return false;
    payload.remove_prefix(5);
  }
  if (pad > payload.size()) return false;
  payload.remove_suffix(pad);
  return true;
}

}  // namespace

Http2Connection::~Http2Connection() { close(); }

bool Http2Connection::connect(std::uint16_t port, std::string* error) {
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (error != nullptr) {
      *error = "nothing answered on 127.0.0.1:" + std::to_string(port) + ": " +
               std::strerror(errno);
    }
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  alive_ = true;
  // Preface, then SETTINGS: no push, the largest stream window, larger frames.
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (::send(fd_, preface, sizeof preface - 1, 0) != static_cast<ssize_t>(sizeof preface - 1)) {
      if (error != nullptr) *error = "could not send the HTTP/2 preface";
      alive_ = false;
      return false;
    }
  }
  std::string settings;
  auto setting = [&](std::uint16_t id, std::uint32_t v) {
    settings.push_back(static_cast<char>(id >> 8));
    settings.push_back(static_cast<char>(id & 0xff));
    put32(settings, v);
  };
  setting(0x2, 0);             // ENABLE_PUSH
  setting(0x4, kMaxWindow);    // INITIAL_WINDOW_SIZE
  setting(0x5, kOurMaxFrame);  // MAX_FRAME_SIZE
  std::string grow;
  put32(grow, kMaxWindow - 65535);
  if (!write_frame(kSettings, 0, 0, settings) || !write_frame(kWindowUpdate, 0, 0, grow)) {
    if (error != nullptr) *error = "could not send SETTINGS";
    alive_ = false;
    return false;
  }
  reader_ = std::thread([this] { read_loop(); });
  return true;
}

bool Http2Connection::write_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                                  std::string_view payload) {
  std::string frame;
  frame.reserve(9 + payload.size());
  const auto len = static_cast<std::uint32_t>(payload.size());
  frame.push_back(static_cast<char>((len >> 16) & 0xff));
  frame.push_back(static_cast<char>((len >> 8) & 0xff));
  frame.push_back(static_cast<char>(len & 0xff));
  frame.push_back(static_cast<char>(type));
  frame.push_back(static_cast<char>(flags));
  put32(frame, stream & 0x7fffffff);
  frame.append(payload.data(), payload.size());
  std::lock_guard<std::mutex> lock(write_mu_);
  if (fd_ < 0) return false;
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

std::uint32_t Http2Connection::open_stream(const Headers& headers, std::string_view body,
                                           bool end_stream, Http2StreamHandlers handlers) {
  if (!alive_) return 0;
  // Ids must be sent in increasing order, so allocation and the HEADERS write
  // happen under one lock.
  std::uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    id = next_stream_;
    next_stream_ += 2;
    streams_[id] = std::make_shared<Http2StreamHandlers>(std::move(handlers));
    const std::string block = hpack_encode(headers);
    const bool headers_end = body.empty() && end_stream;
    if (!write_frame(kHeaders, static_cast<std::uint8_t>(kEndHeaders | (headers_end ? kEndStream : 0)),
                     id, block)) {
      streams_.erase(id);
      return 0;
    }
  }
  std::size_t off = 0;
  while (off < body.size()) {
    const std::size_t n = std::min(peer_max_frame_, body.size() - off);
    const bool last = off + n == body.size();
    if (!write_frame(kData, last && end_stream ? kEndStream : 0, id, body.substr(off, n))) {
      return 0;
    }
    off += n;
  }
  return id;
}

void Http2Connection::cancel_stream(std::uint32_t id) {
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    if (streams_.erase(id) == 0) return;
  }
  std::string code;
  put32(code, 0x8);  // CANCEL
  write_frame(kRstStream, 0, id, code);
}

std::string Http2Connection::close_reason() const {
  std::lock_guard<std::mutex> lock(streams_mu_);
  return reason_;
}

void Http2Connection::fail_all(const std::string& why) {
  std::map<std::uint32_t, std::shared_ptr<Http2StreamHandlers>> dropped;
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    if (reason_.empty()) reason_ = why;
    dropped.swap(streams_);
  }
  alive_ = false;
  for (auto& [id, h] : dropped) {
    if (h->on_aborted) h->on_aborted(why);
  }
}

void Http2Connection::close() {
  {
    std::lock_guard<std::mutex> lock(streams_mu_);
    if (reason_.empty() && fd_ >= 0) reason_ = "the connection was closed by this side";
  }
  if (fd_ >= 0) {
    alive_ = false;
    ::shutdown(fd_, SHUT_RDWR);
  }
  if (reader_.joinable()) reader_.join();
  if (fd_ >= 0) {
    std::lock_guard<std::mutex> lock(write_mu_);
    ::close(fd_);
    fd_ = -1;
  }
  fail_all("the connection was closed by this side");
}

void Http2Connection::read_loop() {
  std::string header_block;
  std::uint32_t header_stream = 0;
  bool header_end_stream = false;
  std::vector<std::uint8_t> payload;
  for (;;) {
    std::uint8_t h[9];
    if (!read_exact(fd_, h, 9)) {
      fail_all("the emulator closed the connection");
      return;
    }
    const std::uint32_t len = (static_cast<std::uint32_t>(h[0]) << 16) |
                              (static_cast<std::uint32_t>(h[1]) << 8) | h[2];
    const std::uint8_t type = h[3], flags = h[4];
    const std::uint32_t stream = get32(h + 5) & 0x7fffffff;
    if (len > kOurMaxFrame) {
      fail_all("the peer sent a frame larger than this side allows");
      return;
    }
    payload.resize(len);
    if (len > 0 && !read_exact(fd_, payload.data(), len)) {
      fail_all("the emulator closed the connection mid-frame");
      return;
    }
    std::string_view p(reinterpret_cast<const char*>(payload.data()), len);

    auto handlers_for = [&](std::uint32_t id) {
      std::lock_guard<std::mutex> lock(streams_mu_);
      const auto it = streams_.find(id);
      return it == streams_.end() ? nullptr : it->second;
    };
    auto finish = [&](std::uint32_t id) {
      std::lock_guard<std::mutex> lock(streams_mu_);
      streams_.erase(id);
    };

    switch (type) {
      case kSettings:
        if ((flags & kAck) == 0) {
          for (std::size_t i = 0; i + 6 <= p.size(); i += 6) {
            const auto id = static_cast<std::uint16_t>(
                (static_cast<std::uint8_t>(p[i]) << 8) | static_cast<std::uint8_t>(p[i + 1]));
            const auto v = get32(reinterpret_cast<const std::uint8_t*>(p.data() + i + 2));
            if (id == 0x5) peer_max_frame_ = v;
          }
          write_frame(kSettings, kAck, 0, {});
        }
        break;
      case kPing:
        if ((flags & kAck) == 0) write_frame(kPing, kAck, 0, p);
        break;
      case kGoAway:
        fail_all("the emulator ended the connection (GOAWAY)");
        return;
      case kWindowUpdate:
      case kPriority:
      case kPushPromise:  // disabled in SETTINGS; ignored if sent anyway
        break;
      case kRstStream: {
        auto hs = handlers_for(stream);
        finish(stream);
        if (hs && hs->on_aborted) {
          hs->on_aborted("the emulator reset the stream (error " +
                         std::to_string(p.size() >= 4
                                            ? get32(reinterpret_cast<const std::uint8_t*>(p.data()))
                                            : 0) +
                         ")");
        }
        break;
      }
      case kHeaders:
      case kContinuation: {
        if (type == kHeaders) {
          if (!unpad(type, flags, p)) {
            fail_all("a malformed HEADERS frame");
            return;
          }
          header_block.assign(p.data(), p.size());
          header_stream = stream;
          header_end_stream = (flags & kEndStream) != 0;
        } else {
          header_block.append(p.data(), p.size());
        }
        if ((flags & kEndHeaders) == 0) break;
        Headers headers;
        if (!decoder_.decode(header_block, headers)) {
          fail_all("could not decode response headers: " + decoder_.error());
          return;
        }
        auto hs = handlers_for(header_stream);
        if (header_end_stream) finish(header_stream);
        if (hs && hs->on_headers) hs->on_headers(headers, header_end_stream);
        break;
      }
      case kData: {
        // Give the window back before anything else, counting the padding too.
        if (len > 0) {
          std::string inc;
          put32(inc, len);
          write_frame(kWindowUpdate, 0, 0, inc);
          if ((flags & kEndStream) == 0) write_frame(kWindowUpdate, 0, stream, inc);
        }
        if (!unpad(type, flags, p)) {
          fail_all("a malformed DATA frame");
          return;
        }
        const bool end = (flags & kEndStream) != 0;
        auto hs = handlers_for(stream);
        if (end) finish(stream);
        if (hs && hs->on_data) hs->on_data(p, end);
        break;
      }
      default:
        break;  // unknown frame types are ignored, as RFC 9113 requires
    }
  }
}

}  // namespace mpi::net
