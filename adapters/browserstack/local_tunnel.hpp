// BrowserStack Local: a tunnel that lets BrowserStack's real devices reach
// this Mac -- localhost, a staging server on the LAN, or Metro on :8081, so a
// React Native debug build can load its bundle from here and DevX's Inspect
// tab can read it through that same Metro.
//
// The tunnel is BrowserStack's own binary, downloaded on request from
// browserstack.com into DevX's folder. BrowserStack publishes it for Intel
// only, so on Apple Silicon it runs under Rosetta 2; where Rosetta is missing
// DevX says so and does not install it. The binary takes the access key as a
// command-line argument (`--key`), which is BrowserStack's design: while the
// tunnel runs, the key is visible to other processes on this Mac.
#pragma once

#include <string>

#include "adapters/browserstack/browserstack.hpp"
#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::browserstack {

std::string local_binary_path();
bool local_binary_installed();
/// True on Intel, and on Apple Silicon when Rosetta 2 can run x86_64 code.
bool can_run_intel_binary();

struct LocalReply {
  bool ok = false;
  std::string state;    // as the binary reports it: "connected", "disconnected", ...
  std::string message;
  std::string error;
  json::Value to_json() const;
};

/// Downloads and unpacks the binary (about 10 MB).
LocalReply install_local_binary(const CancellationToken& cancel);
/// `identifier` names this tunnel, so a session can ask for it
/// (`local-identifier`) and two tunnels on one account do not collide.
LocalReply start_local(const Credentials& c, const std::string& identifier);
LocalReply stop_local(const Credentials& c, const std::string& identifier);

}  // namespace mpi::browserstack
