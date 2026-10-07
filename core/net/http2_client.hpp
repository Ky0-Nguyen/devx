// An HTTP/2 client over plaintext loopback TCP ("h2c with prior knowledge").
//
// It exists for one peer: the Android emulator's gRPC server, which listens on
// 127.0.0.1 and speaks HTTP/2 without TLS. Nothing here negotiates TLS or ALPN,
// and the connection refuses any address but loopback.
//
// One connection carries many streams at once -- a screen stream that never
// ends beside short calls for touch and keys -- so a reader thread owns the
// socket's input and hands each frame to its stream's handlers, and writes are
// serialised by a mutex.
//
// Flow control: this side advertises the largest window HTTP/2 allows and
// tops it up as data is consumed, because a screen stream is megabytes a
// second and a stalled window would look like a frozen device. Requests this
// client sends are small, so it does not wait on the peer's window beyond
// splitting DATA at the peer's maximum frame size.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/net/hpack.hpp"

namespace mpi::net {

struct Http2StreamHandlers {
  /// Response headers, and trailers (a second HEADERS) -- `end_stream` says
  /// which was last.
  std::function<void(const Headers&, bool end_stream)> on_headers;
  std::function<void(std::string_view data, bool end_stream)> on_data;
  /// The stream ended without an END_STREAM from the peer: reset, connection
  /// lost, or closed by this side. Not called after a clean end.
  std::function<void(const std::string& why)> on_aborted;
};

class Http2Connection {
 public:
  Http2Connection() = default;
  ~Http2Connection();
  Http2Connection(const Http2Connection&) = delete;
  Http2Connection& operator=(const Http2Connection&) = delete;

  /// Connects to 127.0.0.1:`port`, sends the preface and SETTINGS, and starts
  /// the reader. False with `error` filled when the peer is not there.
  bool connect(std::uint16_t port, std::string* error);

  /// Opens a stream with `headers`, then `body` as DATA. Returns the stream id,
  /// or 0 when the connection is closed.
  std::uint32_t open_stream(const Headers& headers, std::string_view body,
                            bool end_stream, Http2StreamHandlers handlers);
  /// Sends RST_STREAM(CANCEL). The handlers are dropped without being called.
  void cancel_stream(std::uint32_t id);

  bool alive() const { return alive_.load(); }
  /// Why the connection ended, once it has.
  std::string close_reason() const;
  void close();

 private:
  void read_loop();
  bool write_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                   std::string_view payload);
  void fail_all(const std::string& why);

  int fd_ = -1;
  std::atomic<bool> alive_{false};
  std::thread reader_;
  mutable std::mutex write_mu_;
  mutable std::mutex streams_mu_;
  std::map<std::uint32_t, std::shared_ptr<Http2StreamHandlers>> streams_;
  std::uint32_t next_stream_ = 1;
  std::size_t peer_max_frame_ = 16384;
  HpackDecoder decoder_{4096};
  std::string reason_;
};

}  // namespace mpi::net
