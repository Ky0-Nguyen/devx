// A WebSocket client, enough of RFC 6455 to speak to a debugger.
//
// It exists because the only way to read a React Native app's network calls,
// console output and Redux state *without adding anything to the app* is the
// inspector the app already runs: a debug build connects itself to Metro, and
// Metro proxies a Chrome DevTools Protocol session over a WebSocket. There is
// no HTTP form of that protocol to fall back on.
//
// Written here rather than pulled in, for the reason ADR-0002 gives. It is
// deliberately partial, and the omissions are listed rather than discovered:
//
//   * client only -- it never accepts a connection;
//   * loopback only, like the server next door. Metro runs on the developer's
//     own machine, so there is nothing to gain from allowing more and a great
//     deal to lose;
//   * text frames and continuations; binary frames are read and discarded,
//     because CDP is JSON;
//   * no extensions, so no permessage-deflate. The inspector does not ask for
//     one, and a negotiated extension we cannot decode would be a silent
//     corruption rather than an error -- so an extension in the response is
//     refused outright;
//   * outgoing frames are never fragmented. CDP commands are small.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"

namespace mpi::net {

/// Why a read returned.
///
/// A timeout is not an error and not a closed socket: on a quiet app it is the
/// normal outcome, and a caller that treated it as failure would report an
/// idle app as a broken connection.
enum class WsRead {
  kMessage,   // `message` holds a complete text message
  kTimeout,   // nothing arrived within the deadline; the connection is fine
  kClosed,    // the peer closed, cleanly or otherwise
  kError,     // protocol or socket failure; `error` says what
  kCancelled, // the caller's cancel token was set
};

class WebSocketClient {
 public:
  WebSocketClient() = default;
  ~WebSocketClient();
  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  /// Connects to `path` on 127.0.0.1:`port` and performs the upgrade.
  ///
  /// The server's `Sec-WebSocket-Accept` is verified. It is not a security
  /// control -- nothing here is authenticated and the peer is on loopback --
  /// but it is the only thing that distinguishes a real WebSocket peer from
  /// any other service that happens to answer 101, and reading frames from
  /// something that is not framing them would produce garbage rather than a
  /// diagnosis.
  bool connect(std::uint16_t port, const std::string& path, std::string* error);

  /// Reads one complete text message, waiting at most `timeout_ms`.
  ///
  /// Continuation frames are reassembled. Control frames are handled here --
  /// a ping is answered with a pong, because a peer that stops hearing from
  /// us will drop the session mid-capture.
  WsRead read(std::string* message, int timeout_ms, std::string* error,
              const CancellationToken* cancel = nullptr);

  bool send_text(const std::string& payload, std::string* error);

  /// Sends a close frame and shuts the socket down. Safe to call twice.
  void close();
  bool connected() const { return fd_ >= 0; }

  // --- exposed for tests -------------------------------------------------
  //
  // The frame codec and the handshake hash are pure functions with exact
  // answers, and they are the two places where a mistake is invisible until
  // it corrupts a capture. Tested directly rather than through a socket.

  /// Encodes one unfragmented frame. `mask` must be 4 bytes.
  static std::string encode_frame(std::uint8_t opcode, const std::string& payload,
                                  const unsigned char mask[4]);
  /// The `Sec-WebSocket-Accept` value a server must return for `key`.
  static std::string handshake_accept(const std::string& key);
  static std::string base64(const unsigned char* data, std::size_t len);
  /// SHA-1, used only to compute the handshake value above. It is not used to
  /// protect anything, and must not be: it is broken for that purpose.
  static void sha1(const unsigned char* data, std::size_t len,
                   unsigned char out[20]);

 private:
  WsRead pump(int timeout_ms, std::string* error, const CancellationToken* cancel);
  bool write_all(const std::string& bytes, std::string* error);

  int fd_ = -1;
  std::string inbox_;        // bytes read but not yet framed
  std::string assembling_;   // payload of a fragmented message in progress
  std::uint8_t continued_opcode_ = 0;
  bool sent_close_ = false;
};

}  // namespace mpi::net
