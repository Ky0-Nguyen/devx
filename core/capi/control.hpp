// The channel through which an AI tool, by way of `mpi mcp`, can ask a running
// DevX window to show something -- a session, an issue, the timeline, a layout
// snapshot, a device -- and read what the window is showing.
//
// DevX listens on 127.0.0.1 only, on a port chosen at start, behind a token
// made at start. Both go into a file only this user can read
// (~/Library/Application Support/DevX/control.json, 0600), which is how
// `mpi mcp` finds the window. Nothing here measures or changes a device: a
// command changes what the window shows, and the window says on screen that
// an AI tool did it.
#pragma once

#include <optional>
#include <string>

#include "core/util/json.hpp"

namespace mpi::capi {

std::string control_file_path();

struct ControlEndpoint {
  int port = 0;
  std::string token;
  int pid = 0;
};
/// The running window's endpoint, or nothing when DevX is not running (the
/// file is absent, or names a process that has exited).
std::optional<ControlEndpoint> find_control_endpoint();

/// GET `path_and_query` (without the token) on the window's endpoint.
/// Returns the JSON reply, or an object with `error`.
json::Value control_request(const std::string& path_and_query, int timeout_ms);

}  // namespace mpi::capi
