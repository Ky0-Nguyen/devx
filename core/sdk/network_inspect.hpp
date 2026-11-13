// Inspect for a build that runs no inspector.
//
// A release build has no debugger, so `mpi inspect` and DevX's Inspect tab
// have nothing to attach to. The in-app SDK's `trackNetwork()` wraps the
// app's own fetch and XMLHttpRequest and sends what they did here, as the
// same Network events a debugger would have received, plus the response
// bodies a debugger would have asked for. They go into the same
// `InspectAssembler`, so a release build's requests read exactly like a debug
// build's: the same rows, headers, bodies and GraphQL operations.
//
// The listener is the SDK bridge's kind: 127.0.0.1 only, and every request
// carries a token. Unlike a capture's bridge, the port and token can be fixed,
// because a release build has its endpoint built in and cannot be told a new
// one each run.
//
// Routes, both POST:
//
//   /sdk/v1/network/hello   who is sending: app, version, platform, device
//   /sdk/v1/network         a batch of events, with a sequence number
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/observe/inspect.hpp"
#include "core/sdk/marker_ingest.hpp"
#include "core/util/json.hpp"

namespace mpi::sdk {

struct NetworkInspectLimits {
  std::size_t max_events_per_batch = 2000;
  // Requests kept for one listening session. Past this, new requests are
  // counted and dropped rather than growing without limit; the report says
  // how many.
  std::size_t max_requests = 5000;
  std::size_t max_string_length = 256;
};

// What the app said about itself.
struct NetworkHello {
  std::string app_identifier;
  std::string app_version;
  std::string build_configuration;
  std::string platform;   // "ios" | "android" | ...
  std::string device;
  std::string sdk_version;
  bool detail = false;    // whether headers and bodies are being sent
};

class NetworkInspect {
 public:
  explicit NetworkInspect(NetworkInspectLimits limits = NetworkInspectLimits());
  ~NetworkInspect();
  NetworkInspect(const NetworkInspect&) = delete;
  NetworkInspect& operator=(const NetworkInspect&) = delete;

  // Binds 127.0.0.1:`port` (0 takes an ephemeral one) and serves on a
  // background thread. `token` empty means a generated one; otherwise it is
  // used as given, and refused if it is too weak to guard the listener.
  bool start(std::uint16_t port, const std::string& token, std::string* error);
  void stop();
  bool running() const;

  std::uint16_t port() const;
  const std::string& token() const;
  std::string adb_reverse_command() const;

  // The two routes, callable without a socket so they can be tested.
  IngestResult receive_hello(const json::Value& body);
  IngestResult receive_events(const json::Value& body);

  // Everything so far, as an Inspect report.
  observe::InspectReport snapshot() const;
  std::optional<NetworkHello> hello() const;

 private:
  struct Impl;
  NetworkInspectLimits limits_;
  mutable std::mutex mutex_;
  observe::InspectAssembler assembler_;
  std::optional<NetworkHello> hello_;
  std::chrono::steady_clock::time_point started_;
  std::int64_t next_sequence_ = 1;
  std::int64_t missing_batches_ = 0;
  std::int64_t dropped_by_app_ = 0;
  std::int64_t dropped_here_ = 0;
  std::int64_t rejected_events_ = 0;
  std::size_t requests_ = 0;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mpi::sdk
