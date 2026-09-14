// The app SDK's side of the wire.
//
// An in-app SDK is the only source for things no device provider can see: which
// screen mounted, which navigation was cancelled, which interaction the user
// started, and what build the JS bundle actually came from. Spec section 11
// requires those markers and forbids guessing screens from function names, so
// without this the screen and interaction fields simply stay empty.
//
// Three properties shape this file.
//
// It is authenticated and bounded, because spec section 14 forbids an
// unauthenticated listener and a device can send as fast as it likes. The
// token check belongs to the HTTP layer; the size, count and rate limits
// belong here.
//
// It counts what it loses. Every batch carries a sequence number, so a gap
// means markers that were sent and never arrived. That is recorded as missing
// evidence rather than left to look like an app that did nothing -- the same
// rule the collectors follow.
//
// It does not trust the app. Timestamps, names and screen labels come from a
// process this tool does not control, so they are range-checked, length-capped
// and never used to construct a path or a command.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/model/trace.hpp"
#include "core/util/json.hpp"

namespace mpi::sdk {

struct IngestLimits {
  // One batch. A device that wants to send more sends more batches.
  std::size_t max_body_bytes = 1ull * 1024 * 1024;
  std::size_t max_markers_per_batch = 512;
  std::size_t max_string_length = 512;
  // The whole session. Past this the endpoint starts refusing with a retry
  // hint rather than growing without limit (spec J07).
  std::size_t max_markers_per_session = 200000;
  // A marker timestamp this far outside the capture window is not placed on
  // the timeline; an app's clock can be wrong and a stray value would stretch
  // the window over hours.
  model::TimeNs max_clock_skew_ns = 60ll * 1000 * 1000 * 1000;
};

// What the app said about itself at handshake time. Every field is optional:
// an SDK that knows less says less, and unknown stays unknown.
struct Handshake {
  std::string app_identifier;
  std::string sdk_version;
  std::string app_version;
  std::string build_number;
  std::string build_configuration;  // e.g. "Debug", "Release", "Staging"
  std::string js_engine;            // "hermes" | "jsc" | ...
  std::string js_engine_version;
  std::string react_native_version;
  std::string architecture;         // "fabric" | "paper"
  std::string js_bundle_id;         // hash or build id of the loaded bundle
  std::string ota_update_id;        // OTA/CodePush update identity
  std::string native_build_id;
  std::optional<bool> js_dev_mode;
  std::optional<bool> native_debuggable;
  // The app's own clock reading at handshake, used to notice a skewed clock
  // rather than to correct one.
  std::optional<model::TimeNs> app_clock_ns;
  std::string clock_domain;
};

struct IngestResult {
  bool accepted = false;
  int http_status = 200;
  std::int64_t markers_accepted = 0;
  std::int64_t markers_rejected = 0;
  // Set when the caller should back off instead of retrying immediately.
  std::optional<int> retry_after_ms;
  std::string error;
  std::vector<std::string> warnings;
  // The sequence number the endpoint expects next, so a client that lost a
  // response can resynchronise instead of duplicating.
  std::int64_t next_sequence = 0;
};

// Accumulates what the app reported during one capture.
//
// Thread-safe: the HTTP server handles connections on the calling thread and
// a live capture reads the accumulated markers from another.
class MarkerIngest {
 public:
  explicit MarkerIngest(IngestLimits limits = IngestLimits());

  // The handshake. Returns the facts it derived, which the caller merges into
  // the trace's build profile.
  IngestResult receive_handshake(const json::Value& body);

  // One batch of markers.
  IngestResult receive_markers(const json::Value& body);

  // Everything received so far, in arrival order.
  std::vector<model::Marker> take_markers();
  // Build facts derived from the handshake.
  std::vector<model::BuildFact> build_facts() const;
  // What the SDK reported about itself, for the capability record.
  std::optional<Handshake> handshake() const;

  // The pairing that makes a measured clock mapping possible: the app's own
  // clock reading from the handshake, and the host instant it arrived at.
  struct ClockPairing {
    model::TimeNs app_clock_ns = 0;
    std::chrono::steady_clock::time_point host_received;
    // How long the host spent between reading its clock and finishing the
    // request, which bounds how precisely the two can be paired.
    std::chrono::nanoseconds host_uncertainty{0};
    std::string app_domain;
    bool valid = false;
  };
  ClockPairing clock_pairing() const;

  // Sequence gaps observed: markers the app says it sent that never arrived.
  std::int64_t missing_batches() const;
  std::int64_t duplicate_batches() const;
  std::int64_t markers_dropped_by_app() const;
  std::int64_t markers_rejected() const;
  std::vector<std::string> notes() const;

  // A capability record describing what the SDK contributed, including what
  // it lost. Empty statuses are never reported as success.
  model::Capability capability() const;

 private:
  IngestLimits limits_;
  mutable std::mutex mutex_;
  std::optional<Handshake> handshake_;
  std::vector<model::Marker> markers_;
  std::int64_t next_sequence_ = 1;
  std::int64_t missing_batches_ = 0;
  std::int64_t duplicate_batches_ = 0;
  std::int64_t markers_dropped_by_app_ = 0;
  std::int64_t markers_rejected_ = 0;
  std::vector<std::string> notes_;
  ClockPairing pairing_;
};

// The marker kinds the SDK may send. Anything else is rejected rather than
// stored under a name no rule will ever look for.
bool is_known_marker_kind(const std::string& kind);

}  // namespace mpi::sdk
