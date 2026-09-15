// A one-shot HTTP GET against 127.0.0.1.
//
// The inspector's target list lives at `http://localhost:<metro>/json/list`,
// and reaching it is the only HTTP request this tool makes. Loopback only,
// like the server and the websocket client next door: Metro runs on the
// developer's own machine, so there is nothing to gain from allowing more.
#pragma once

#include <cstdint>
#include <string>

namespace mpi::net {

struct GetResult {
  bool ok = false;
  int status = 0;          // 0 when the request never got a status line
  std::string body;
  std::string error;
};

/// Performs `GET path` on 127.0.0.1:`port`, waiting at most `timeout_ms`.
///
/// Reads a Content-Length body, or to end-of-stream when the server does not
/// say. Chunked encoding is refused rather than mis-parsed: Metro does not
/// use it for this endpoint, and a half-understood body would be worse than
/// an error.
GetResult loopback_get(std::uint16_t port, const std::string& path,
                       int timeout_ms = 3000,
                       std::size_t max_bytes = 4u * 1024 * 1024);

}  // namespace mpi::net
