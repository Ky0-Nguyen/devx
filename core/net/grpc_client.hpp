// gRPC over the HTTP/2 client: unary calls and server streams.
//
// Messages are protobuf bytes the caller builds with core/util/protobuf; this
// layer adds the five-byte gRPC framing, the request headers, and reads the
// status the server ends every call with. A call that fails says how: the
// gRPC status and message when the server sent one, or what happened to the
// connection when it did not.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/net/http2_client.hpp"

namespace mpi::net {

struct GrpcStatus {
  /// The gRPC status code; -1 when the call never got one (connection lost,
  /// timed out, cancelled).
  int code = -1;
  std::string message;
  bool ok() const { return code == 0; }
};

class GrpcStream;

class GrpcChannel {
 public:
  /// `authorization` is the whole header value, e.g. "Bearer <token>"; empty
  /// for none.
  GrpcChannel(std::uint16_t port, std::string authorization);
  bool connect(std::string* error);
  bool alive() const { return conn_.alive(); }

  /// One request, one response. `path` is "/package.Service/Method".
  GrpcStatus unary(const std::string& path, std::string_view request, std::string* response,
                   std::chrono::milliseconds timeout);

  /// A server stream: `on_message` runs on the connection's reader thread for
  /// every message, so it must be quick. The returned handle cancels it.
  std::shared_ptr<GrpcStream> server_stream(const std::string& path, std::string_view request,
                                            std::function<void(std::string_view)> on_message);

  void close() { conn_.close(); }

 private:
  Headers request_headers(const std::string& path) const;
  std::uint16_t port_;
  std::string auth_;
  Http2Connection conn_;
  friend class GrpcStream;
};

class GrpcStream {
 public:
  void cancel();
  bool finished() const;
  /// Waits for the stream to end, up to `timeout`; true when it did.
  bool wait(std::chrono::milliseconds timeout);
  GrpcStatus status() const;

 private:
  friend class GrpcChannel;
  void feed(std::string_view data);
  void headers(const Headers& h, bool end);
  void end(int code, std::string message);

  GrpcChannel* channel_ = nullptr;
  std::uint32_t id_ = 0;
  std::function<void(std::string_view)> on_message_;
  std::string buffer_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  GrpcStatus status_;
};

/// The gRPC framing, exposed for the tests: one message, five-byte prefix.
std::string grpc_frame(std::string_view message);

}  // namespace mpi::net
