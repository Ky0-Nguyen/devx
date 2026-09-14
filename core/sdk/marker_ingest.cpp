#include "core/sdk/marker_ingest.hpp"

#include <algorithm>

namespace mpi::sdk {
namespace {

// Spec section 11 names these: screen mount/unmount, navigation
// begin/end/cancel, interactions, async spans. A cancelled navigation is its
// own kind because "cancelled" and "never finished" are different facts (G07).
const char* const kKinds[] = {
    "screen_mount",      "screen_unmount",     "navigation_begin",
    "navigation_end",    "navigation_cancel",  "interaction_begin",
    "interaction_end",   "async_span_begin",   "async_span_end",
    "react_commit",      "network_request",    "runtime_reload",
};

std::string clamp_string(const std::string& in, std::size_t max) {
  if (in.size() <= max) return in;
  return in.substr(0, max);
}

// A marker's identity, for the dedup a retrying client makes necessary.
std::string marker_key(const model::Marker& m) {
  return m.kind + "|" + m.screen + "|" + m.interaction + "|" +
         std::to_string(m.timestamp_ns);
}

}  // namespace

bool is_known_marker_kind(const std::string& kind) {
  for (const char* k : kKinds) {
    if (kind == k) return true;
  }
  return false;
}

MarkerIngest::MarkerIngest(IngestLimits limits) : limits_(limits) {}

IngestResult MarkerIngest::receive_handshake(const json::Value& body) {
  IngestResult result;
  std::lock_guard<std::mutex> lock(mutex_);

  if (!body.is_object()) {
    result.http_status = 400;
    result.error = "handshake body must be a JSON object";
    return result;
  }

  const auto str = [&](const char* key) -> std::string {
    const auto* v = body.find(key);
    if (v == nullptr || !v->is_string()) return {};
    return clamp_string(v->as_string(), limits_.max_string_length);
  };
  const auto tri = [&](const char* key) -> std::optional<bool> {
    const auto* v = body.find(key);
    if (v == nullptr || !v->is_bool()) return std::nullopt;
    return v->as_bool();
  };

  Handshake h;
  h.app_identifier = str("app_identifier");
  h.sdk_version = str("sdk_version");
  h.app_version = str("app_version");
  h.build_number = str("build_number");
  h.build_configuration = str("build_configuration");
  h.js_engine = str("js_engine");
  h.js_engine_version = str("js_engine_version");
  h.react_native_version = str("react_native_version");
  h.architecture = str("architecture");
  h.js_bundle_id = str("js_bundle_id");
  h.ota_update_id = str("ota_update_id");
  h.native_build_id = str("native_build_id");
  h.js_dev_mode = tri("js_dev_mode");
  h.native_debuggable = tri("native_debuggable");
  h.clock_domain = str("clock_domain");
  if (const auto* v = body.find("app_clock_ns")) {
    if (v->is_number()) {
      h.app_clock_ns = static_cast<model::TimeNs>(v->as_int());
    }
  }

  if (h.sdk_version.empty()) {
    result.http_status = 400;
    result.error =
        "handshake must state sdk_version: a marker stream whose producer is "
        "unidentified cannot be interpreted later";
    return result;
  }

  if (handshake_.has_value() && handshake_->js_bundle_id != h.js_bundle_id) {
    // The app reloaded onto a different bundle mid-capture. Both halves are
    // real, and mixing them silently would attribute one bundle's work to the
    // other (spec G04, G15).
    notes_.push_back(
        "the app handshook again with a different JS bundle id ('" +
        handshake_->js_bundle_id + "' then '" + h.js_bundle_id +
        "'): markers before and after that point came from different bundles "
        "and are not interchangeable");
  }
  handshake_ = h;
  result.accepted = true;
  result.next_sequence = next_sequence_;
  return result;
}

IngestResult MarkerIngest::receive_markers(const json::Value& body) {
  IngestResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  result.next_sequence = next_sequence_;

  if (!handshake_.has_value()) {
    // Without a handshake there is no producer identity, and a marker whose
    // origin is unknown is not evidence about any build.
    result.http_status = 409;
    result.error =
        "no handshake has been received: POST /sdk/v1/hello first so the "
        "markers can be attributed to a build";
    return result;
  }
  if (!body.is_object()) {
    result.http_status = 400;
    result.error = "batch body must be a JSON object";
    return result;
  }
  if (markers_.size() >= limits_.max_markers_per_session) {
    // Backpressure rather than unbounded growth (spec J07). The client is
    // told to back off; nothing is silently dropped on this side.
    result.http_status = 429;
    result.retry_after_ms = 1000;
    result.error = "this session has reached its marker limit of " +
                   std::to_string(limits_.max_markers_per_session) +
                   "; the capture is still running and later markers will be "
                   "missing from it";
    notes_.push_back(
        "the session marker limit was reached, so markers after that point "
        "were refused: the later part of the capture has no SDK evidence");
    return result;
  }

  const auto* seq_value = body.find("sequence");
  if (seq_value == nullptr || !seq_value->is_number()) {
    result.http_status = 400;
    result.error =
        "batch must carry a numeric `sequence`: without it a lost batch is "
        "indistinguishable from an app that sent nothing";
    return result;
  }
  const auto sequence = seq_value->as_int();
  if (sequence < next_sequence_) {
    // A client that retried after a lost response. Accepting it again would
    // double-count; refusing it silently would look like data loss.
    ++duplicate_batches_;
    result.accepted = true;
    result.http_status = 200;
    result.warnings.push_back(
        "batch " + std::to_string(sequence) +
        " was already received; it was not stored twice");
    result.next_sequence = next_sequence_;
    return result;
  }
  if (sequence > next_sequence_) {
    const auto missing = sequence - next_sequence_;
    missing_batches_ += missing;
    notes_.push_back(
        std::to_string(missing) + " batch(es) between sequence " +
        std::to_string(next_sequence_) + " and " + std::to_string(sequence) +
        " never arrived: those markers are missing evidence, not an absence "
        "of activity");
    result.warnings.push_back("gap of " + std::to_string(missing) +
                              " batch(es) recorded before this one");
  }

  // The app reports what its own buffer dropped, which is the only way to
  // know about markers that never reached the wire.
  if (const auto* dropped = body.find("dropped_by_app")) {
    if (dropped->is_number()) {
      const auto n = dropped->as_int();
      if (n > 0) {
        markers_dropped_by_app_ += n;
        notes_.push_back(
            "the app reported dropping " + std::to_string(n) +
            " marker(s) from its own buffer before sending");
      }
    }
  }

  const auto* array = body.find("markers");
  if (array == nullptr || !array->is_array()) {
    result.http_status = 400;
    result.error = "batch must carry a `markers` array";
    return result;
  }
  if (array->items().size() > limits_.max_markers_per_batch) {
    result.http_status = 413;
    result.error = "batch holds " + std::to_string(array->items().size()) +
                   " markers, over the per-batch limit of " +
                   std::to_string(limits_.max_markers_per_batch);
    return result;
  }

  for (const json::Value& item : array->items()) {
    if (!item.is_object()) {
      ++markers_rejected_;
      ++result.markers_rejected;
      continue;
    }
    const auto* kind = item.find("kind");
    const auto* ts = item.find("timestamp_ns");
    if (kind == nullptr || !kind->is_string() || ts == nullptr ||
        !ts->is_number()) {
      ++markers_rejected_;
      ++result.markers_rejected;
      continue;
    }
    const std::string kind_text =
        clamp_string(kind->as_string(), limits_.max_string_length);
    if (!is_known_marker_kind(kind_text)) {
      // Stored under an unknown name, no rule would ever query it, so it
      // would be invisible rather than useful. Rejecting says so.
      ++markers_rejected_;
      ++result.markers_rejected;
      result.warnings.push_back("unknown marker kind '" + kind_text +
                                "' was rejected");
      continue;
    }

    model::Marker m;
    m.kind = kind_text;
    m.timestamp_ns = static_cast<model::TimeNs>(ts->as_int());
    if (m.timestamp_ns <= 0) {
      ++markers_rejected_;
      ++result.markers_rejected;
      continue;
    }
    if (const auto* d = item.find("duration_ns")) {
      if (d->is_number() && d->as_int() >= 0) {
        m.duration_ns = static_cast<model::TimeNs>(d->as_int());
      }
    }
    if (const auto* screen = item.find("screen")) {
      if (screen->is_string()) {
        m.screen = clamp_string(screen->as_string(), limits_.max_string_length);
      }
    }
    if (const auto* interaction = item.find("interaction")) {
      if (interaction->is_string()) {
        m.interaction =
            clamp_string(interaction->as_string(), limits_.max_string_length);
      }
    }
    if (const auto* payload = item.find("payload")) {
      if (payload->is_object()) m.payload = *payload;
    }
    m.event_id = "sdk-" + std::to_string(markers_.size());
    m.payload.set("sdk_sequence", json::Value::number(
                                      static_cast<double>(sequence)));

    // A retrying client can resend the same marker inside a new batch.
    const auto key = marker_key(m);
    const bool already = std::any_of(
        markers_.begin(), markers_.end(), [&](const model::Marker& existing) {
          return marker_key(existing) == key;
        });
    if (already) {
      ++result.markers_rejected;
      continue;
    }

    markers_.push_back(std::move(m));
    ++result.markers_accepted;
  }

  next_sequence_ = sequence + 1;
  result.next_sequence = next_sequence_;
  result.accepted = true;
  return result;
}

std::vector<model::Marker> MarkerIngest::take_markers() {
  std::lock_guard<std::mutex> lock(mutex_);
  return markers_;
}

std::vector<model::BuildFact> MarkerIngest::build_facts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<model::BuildFact> facts;
  if (!handshake_.has_value()) return facts;
  const Handshake& h = *handshake_;

  const auto add = [&](const char* key, const std::string& value) {
    if (value.empty()) return;
    model::BuildFact f;
    f.key = key;
    f.value = value;
    // Reported by the app at runtime, which is weaker than a build-time
    // manifest and stronger than an inference (spec section 7.2).
    f.source = model::FactSource::kRuntimeSdk;
    f.basis = "reported by the in-app SDK at handshake";
    facts.push_back(std::move(f));
  };
  const auto add_bool = [&](const char* key, const std::optional<bool>& value) {
    if (!value.has_value()) return;
    model::BuildFact f;
    f.key = key;
    f.value = *value ? "true" : "false";
    f.boolean_value = *value ? model::Tri::kTrue : model::Tri::kFalse;
    f.source = model::FactSource::kRuntimeSdk;
    f.basis = "reported by the in-app SDK at handshake";
    facts.push_back(std::move(f));
  };

  add("sdk.version", h.sdk_version);
  add("app.version", h.app_version);
  add("app.build_number", h.build_number);
  add("build.configuration", h.build_configuration);
  add("js.engine", h.js_engine);
  add("js.engine_version", h.js_engine_version);
  add("js.react_native_version", h.react_native_version);
  add("js.architecture", h.architecture);
  add("js.bundle_id", h.js_bundle_id);
  add("js.ota_update_id", h.ota_update_id);
  add("native.build_id", h.native_build_id);
  add_bool("js.__DEV__", h.js_dev_mode);
  add_bool("native.debuggable", h.native_debuggable);
  return facts;
}

std::optional<Handshake> MarkerIngest::handshake() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handshake_;
}

std::int64_t MarkerIngest::missing_batches() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return missing_batches_;
}

std::int64_t MarkerIngest::duplicate_batches() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return duplicate_batches_;
}

std::int64_t MarkerIngest::markers_dropped_by_app() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return markers_dropped_by_app_;
}

std::int64_t MarkerIngest::markers_rejected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return markers_rejected_;
}

std::vector<std::string> MarkerIngest::notes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return notes_;
}

model::Capability MarkerIngest::capability() const {
  std::lock_guard<std::mutex> lock(mutex_);
  model::Capability c;
  c.id = "app.sdk.markers";
  c.human_name = "In-app SDK markers";
  c.provider = "mpi app SDK";

  if (!handshake_.has_value()) {
    c.status = model::CapabilityStatus::kUnsupported;
    c.evidence =
        "no app ever handshook with the SDK endpoint, so this capture has no "
        "screen, navigation or interaction evidence";
    c.limitations.push_back(
        "screen and interaction fields stay empty: they are never guessed "
        "from function names (spec section 11)");
    c.tested = model::TestedState::kNotTested;
    return c;
  }

  c.status = markers_.empty() ? model::CapabilityStatus::kLimited
                              : model::CapabilityStatus::kAvailable;
  c.evidence = "SDK " + handshake_->sdk_version + " sent " +
               std::to_string(markers_.size()) + " marker(s)";
  if (markers_.empty()) {
    c.evidence += "; the app connected and reported nothing";
  }
  if (missing_batches_ > 0) {
    c.limitations.push_back(
        std::to_string(missing_batches_) +
        " batch(es) never arrived, so part of the marker stream is missing "
        "and absence of a marker in that stretch proves nothing");
  }
  if (markers_dropped_by_app_ > 0) {
    c.limitations.push_back(
        "the app dropped " + std::to_string(markers_dropped_by_app_) +
        " marker(s) from its own buffer before sending");
  }
  if (markers_rejected_ > 0) {
    c.limitations.push_back(std::to_string(markers_rejected_) +
                            " marker(s) were rejected as malformed or of an "
                            "unknown kind");
  }
  c.limitations.push_back(
      "markers are the app's own account of itself, on the app's clock; they "
      "are not corroborated by the platform");
  c.tested = model::TestedState::kProbedOnly;
  return c;
}

}  // namespace mpi::sdk
