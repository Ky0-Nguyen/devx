// The SDK's loopback endpoint.
//
// Binds 127.0.0.1 and requires a token on every request, both structurally
// (see `core/net/http_server`): spec section 14 forbids an unauthenticated
// network listener, and an in-app SDK is exactly the kind of client that
// invites one.
//
// The device reaches it without any network exposure:
//
//   Android   adb reverse tcp:<port> tcp:<port>   then http://127.0.0.1:<port>
//   iOS sim   the simulator shares the host's loopback already
//   iOS dev   a USB tunnel; there is no wireless path on purpose
//
// Two routes, both POST:
//
//   /sdk/v1/hello     the build/runtime handshake, once per runtime
//   /sdk/v1/markers   a batch, with a sequence number so a lost batch is a
//                     recorded gap rather than an app that went quiet
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/net/http_server.hpp"
#include "core/sdk/marker_ingest.hpp"
#include "core/util/cancel.hpp"

namespace mpi::sdk {

class SdkBridge {
 public:
  explicit SdkBridge(IngestLimits limits = IngestLimits());
  ~SdkBridge();
  SdkBridge(const SdkBridge&) = delete;
  SdkBridge& operator=(const SdkBridge&) = delete;

  // Binds and starts serving on a background thread. `port` 0 takes an
  // ephemeral port, which is what a capture normally wants.
  bool start(std::uint16_t port, std::string* error);
  void stop();

  std::uint16_t port() const;
  const std::string& token() const;
  // The line an operator runs to let an Android device reach the endpoint.
  std::string adb_reverse_command() const;

  MarkerIngest& ingest() { return ingest_; }
  const MarkerIngest& ingest() const { return ingest_; }

 private:
  struct Impl;
  MarkerIngest ingest_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mpi::sdk
