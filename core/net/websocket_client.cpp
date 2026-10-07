#include "core/net/websocket_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

namespace mpi::net {
namespace {

// RFC 6455 section 1.3. Fixed by the standard, not a choice.
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

/// Four bytes from the system's random device.
///
/// The mask is not a security measure -- RFC 6455 requires it so that
/// intermediaries cannot be confused into treating frame content as a
/// request -- but a predictable mask defeats even that, so it comes from
/// /dev/urandom rather than from rand().
void random_mask(unsigned char out[4]) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (urandom.read(reinterpret_cast<char*>(out), 4)) return;
  // A machine with no /dev/urandom is not a machine this tool runs on, but a
  // silently constant mask would still be wrong: make it obviously varying.
  static std::uint32_t counter = 0x5bf03635u;
  counter = counter * 1664525u + 1013904223u;
  std::memcpy(out, &counter, 4);
}

}  // namespace

void WebSocketClient::sha1(const unsigned char* data, std::size_t len,
                           unsigned char out[20]) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                        0xC3D2E1F0u};
  // The message is padded to a multiple of 64 bytes with a 1 bit, zeroes, and
  // the original length in bits as a big-endian 64-bit value.
  std::size_t total = len + 1 + 8;
  std::size_t padded = ((total + 63) / 64) * 64;
  std::vector<unsigned char> msg(padded, 0);
  std::memcpy(msg.data(), data, len);
  msg[len] = 0x80;
  std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
  for (int i = 0; i < 8; i++) {
    msg[padded - 1 - static_cast<std::size_t>(i)] =
        static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
  }
  for (std::size_t off = 0; off < padded; off += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4]) << 24) |
             (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
      std::uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = (v << 1) | (v >> 31);
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      std::uint32_t f, k;
      if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
      std::uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; i++) {
    out[i * 4]     = static_cast<unsigned char>((h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFF);
  }
}

std::string WebSocketClient::base64(const unsigned char* data, std::size_t len) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += (i + 1 < len) ? kAlphabet[(n >> 6) & 63] : '=';
    out += (i + 2 < len) ? kAlphabet[n & 63] : '=';
  }
  return out;
}

std::string WebSocketClient::handshake_accept(const std::string& key) {
  std::string joined = key + kGuid;
  unsigned char digest[20];
  sha1(reinterpret_cast<const unsigned char*>(joined.data()), joined.size(),
       digest);
  return base64(digest, 20);
}

std::string WebSocketClient::encode_frame(std::uint8_t opcode,
                                          const std::string& payload,
                                          const unsigned char mask[4]) {
  std::string out;
  out += static_cast<char>(0x80 | opcode);   // FIN set: never fragmented
  std::size_t n = payload.size();
  // The three length encodings. A client frame always sets the mask bit, so
  // the length byte carries 0x80 as well.
  if (n < 126) {
    out += static_cast<char>(0x80 | n);
  } else if (n <= 0xFFFF) {
    out += static_cast<char>(0x80 | 126);
    out += static_cast<char>((n >> 8) & 0xFF);
    out += static_cast<char>(n & 0xFF);
  } else {
    out += static_cast<char>(0x80 | 127);
    for (int i = 7; i >= 0; i--) {
      out += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF);
    }
  }
  for (int i = 0; i < 4; i++) out += static_cast<char>(mask[i]);
  for (std::size_t i = 0; i < n; i++) {
    out += static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                             mask[i % 4]);
  }
  return out;
}

WebSocketClient::~WebSocketClient() { close(); }

bool WebSocketClient::connect(std::uint16_t port, const std::string& path,
                              std::string* error) {
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int yes = 1;
  // A peer that has gone (Metro restarting, an app killed) must turn a send
  // into EPIPE, not a SIGPIPE that ends the process before it reports.
  ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  // Loopback, always. See the header.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = "connect 127.0.0.1:" + std::to_string(port) + ": " +
             std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  unsigned char key_bytes[16];
  random_mask(key_bytes);
  random_mask(key_bytes + 4);
  random_mask(key_bytes + 8);
  random_mask(key_bytes + 12);
  std::string key = base64(key_bytes, 16);

  std::string request =
      "GET " + path + " HTTP/1.1\r\n"
      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      // The server's own origin, port included. React Native's inspector
      // proxy refuses a debugger with no loopback Origin (401), and Expo's
      // dev server then drops one whose host:port is not its own.
      "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
  if (!write_all(request, error)) {
    close();
    return false;
  }

  // Read headers only. Anything after the blank line is already frame data
  // and must be kept -- a fast peer can send the first event in the same
  // segment as the handshake response, and discarding it would lose an event
  // with no trace.
  std::string head;
  while (head.find("\r\n\r\n") == std::string::npos) {
    pollfd p{fd_, POLLIN, 0};
    int rc = ::poll(&p, 1, 5000);
    if (rc <= 0) {
      *error = rc == 0 ? "the handshake response did not arrive within 5s"
                       : std::string("poll: ") + std::strerror(errno);
      close();
      return false;
    }
    char buf[4096];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
      *error = "the peer closed during the handshake";
      close();
      return false;
    }
    head.append(buf, static_cast<std::size_t>(n));
  }
  std::size_t split = head.find("\r\n\r\n");
  inbox_ = head.substr(split + 4);
  head.resize(split);

  std::string status = head.substr(0, head.find("\r\n"));
  if (status.find(" 101") == std::string::npos) {
    // A 200 here means the path exists but is not a WebSocket endpoint, which
    // is a different mistake from a 404 -- so the status line is quoted.
    *error = "the peer refused the upgrade: " + status;
    close();
    return false;
  }
  std::string lowered = lower(head);
  if (lowered.find("upgrade: websocket") == std::string::npos) {
    *error = "the peer answered 101 without upgrading to websocket";
    close();
    return false;
  }
  // An extension we cannot decode would corrupt every frame quietly, so it is
  // refused rather than ignored.
  if (lowered.find("sec-websocket-extensions:") != std::string::npos) {
    *error = "the peer negotiated a websocket extension, which this client "
             "cannot decode; refusing rather than reading corrupt frames";
    close();
    return false;
  }
  std::string want = lower("sec-websocket-accept: " + handshake_accept(key));
  if (lowered.find(want) == std::string::npos) {
    *error = "the peer's Sec-WebSocket-Accept did not match the key sent, so "
             "this is not a websocket peer";
    close();
    return false;
  }
  return true;
}

bool WebSocketClient::write_all(const std::string& bytes, std::string* error) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      *error = std::string("send: ") + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      *error = "send returned 0: the peer is gone";
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool WebSocketClient::send_text(const std::string& payload, std::string* error) {
  if (fd_ < 0) {
    *error = "not connected";
    return false;
  }
  unsigned char mask[4];
  random_mask(mask);
  return write_all(encode_frame(kOpText, payload, mask), error);
}

WsRead WebSocketClient::read(std::string* message, int timeout_ms,
                             std::string* error,
                             const CancellationToken* cancel) {
  message->clear();
  if (fd_ < 0) return WsRead::kClosed;

  // Frames already buffered are returned without touching the socket: one
  // recv often carries several events, and going back to poll() first would
  // stall them behind the next arrival.
  for (;;) {
    std::size_t need = 2;
    if (inbox_.size() >= need) {
      unsigned char b0 = static_cast<unsigned char>(inbox_[0]);
      unsigned char b1 = static_cast<unsigned char>(inbox_[1]);
      bool fin = (b0 & 0x80) != 0;
      std::uint8_t opcode = b0 & 0x0F;
      // A server frame must not be masked. One that is means we have lost
      // sync with the stream, and every byte after it is suspect.
      if ((b1 & 0x80) != 0) {
        *error = "the peer sent a masked frame, which a server must not do";
        close();
        return WsRead::kError;
      }
      std::uint64_t len = b1 & 0x7F;
      std::size_t header = 2;
      if (len == 126) {
        need = 4;
        if (inbox_.size() < need) goto fill;
        len = (static_cast<std::uint64_t>(static_cast<unsigned char>(inbox_[2])) << 8) |
              static_cast<unsigned char>(inbox_[3]);
        header = 4;
      } else if (len == 127) {
        need = 10;
        if (inbox_.size() < need) goto fill;
        len = 0;
        for (int i = 0; i < 8; i++) {
          len = (len << 8) |
                static_cast<unsigned char>(inbox_[2 + static_cast<std::size_t>(i)]);
        }
        header = 10;
        // A 64-bit length from a debugger is a bug or an attack; either way
        // allocating on it would be the last thing this process did.
        if (len > (64u << 20)) {
          *error = "the peer announced a frame larger than 64 MiB";
          close();
          return WsRead::kError;
        }
      }
      if (inbox_.size() < header + len) goto fill;

      std::string payload = inbox_.substr(header, static_cast<std::size_t>(len));
      inbox_.erase(0, header + static_cast<std::size_t>(len));

      switch (opcode) {
        case kOpPing: {
          // Answered immediately. A peer that stops hearing from us drops the
          // session, and it drops it mid-capture.
          unsigned char mask[4];
          random_mask(mask);
          std::string ignored;
          write_all(encode_frame(kOpPong, payload, mask), &ignored);
          continue;
        }
        case kOpPong:
          continue;
        case kOpClose:
          close();
          return WsRead::kClosed;
        case kOpBinary:
          // CDP is JSON. A binary frame is not something to guess at.
          continue;
        case kOpText:
        case kOpContinuation: {
          if (opcode == kOpText) {
            assembling_ = payload;
            continued_opcode_ = kOpText;
          } else {
            if (continued_opcode_ != kOpText) {
              // A continuation with nothing to continue, or continuing a
              // binary frame we dropped: neither is a text message.
              assembling_.clear();
              continue;
            }
            assembling_ += payload;
          }
          if (!fin) continue;
          *message = assembling_;
          assembling_.clear();
          continued_opcode_ = 0;
          return WsRead::kMessage;
        }
        default:
          continue;   // a reserved opcode: skipped, not guessed at
      }
    }

  fill:
    if (cancel != nullptr && cancel->cancelled()) return WsRead::kCancelled;
    pollfd p{fd_, POLLIN, 0};
    // Poll in slices so a cancel is noticed promptly even on a long deadline.
    int slice = timeout_ms < 0 ? 200 : (timeout_ms > 200 ? 200 : timeout_ms);
    int rc = ::poll(&p, 1, slice);
    if (rc < 0) {
      if (errno == EINTR) continue;
      *error = std::string("poll: ") + std::strerror(errno);
      return WsRead::kError;
    }
    if (rc == 0) {
      if (timeout_ms >= 0) {
        timeout_ms -= slice;
        if (timeout_ms <= 0) return WsRead::kTimeout;
      }
      continue;
    }
    char buf[16384];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      *error = std::string("recv: ") + std::strerror(errno);
      close();
      return WsRead::kError;
    }
    if (n == 0) {
      close();
      return WsRead::kClosed;
    }
    inbox_.append(buf, static_cast<std::size_t>(n));
  }
}

void WebSocketClient::close() {
  if (fd_ < 0) return;
  if (!sent_close_) {
    sent_close_ = true;
    unsigned char mask[4];
    random_mask(mask);
    std::string ignored;
    // Status 1000, "normal closure", big-endian.
    std::string body;
    body += static_cast<char>(0x03);
    body += static_cast<char>(0xE8);
    write_all(encode_frame(kOpClose, body, mask), &ignored);
  }
  ::shutdown(fd_, SHUT_RDWR);
  ::close(fd_);
  fd_ = -1;
}

}  // namespace mpi::net
