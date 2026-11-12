#include "core/sdk/network_inspect.hpp"

#include <thread>
#include <utility>

#include "core/net/http_server.hpp"
#include "core/util/cancel.hpp"

namespace mpi::sdk {
namespace {

const std::string& text_of(const json::Value& v, std::string_view key) {
  static const std::string empty;
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_string() ? f->as_string() : empty;
}

std::string capped(const json::Value& v, std::string_view key, std::size_t limit) {
  std::string s = text_of(v, key);
  if (s.size() > limit) s.resize(limit);
  return s;
}

// The events a debugger would have received, and the one that stands in for
// the `Network.getResponseBody` reply it would have asked for. Anything else
// is refused: an app that sends console or Runtime events here is not
// something this route models.
bool is_known_event(const std::string& method) {
  return method == "Network.requestWillBeSent" ||
         method == "Network.responseReceived" ||
         method == "Network.loadingFinished" ||
         method == "Network.loadingFailed" || method == "DevX.responseBody";
}

net::Response reply(const IngestResult& result) {
  json::Value body = json::Value::object();
  body.set("accepted", json::Value::boolean(result.accepted));
  body.set("events_accepted",
           json::Value::number(static_cast<double>(result.markers_accepted)));
  body.set("events_rejected",
           json::Value::number(static_cast<double>(result.markers_rejected)));
  body.set("next_sequence",
           json::Value::number(static_cast<double>(result.next_sequence)));
  if (!result.error.empty()) body.set("error", json::Value::string(result.error));
  return net::Response::json(body.dump(), result.http_status);
}

}  // namespace

struct NetworkInspect::Impl {
  net::HttpServer server;
  CancellationSource cancel;
  std::thread thread;
  bool running = false;
};

NetworkInspect::NetworkInspect(NetworkInspectLimits limits)
    : limits_(limits),
      started_(std::chrono::steady_clock::now()),
      impl_(std::make_unique<Impl>()) {
  // Headers and bodies are kept whenever the app sends them. Whether to send
  // them is the app's decision (`trackNetwork({ detail })`), made where the
  // data is; refusing them here would only lose what was already sent.
  assembler_.capture_detail(true);
}

NetworkInspect::~NetworkInspect() { stop(); }

bool NetworkInspect::start(std::uint16_t port, const std::string& token,
                           std::string* error) {
  if (impl_->running) {
    if (error != nullptr) *error = "already listening";
    return false;
  }
  if (!token.empty() && !impl_->server.set_token(token)) {
    if (error != nullptr) {
      *error = "the token must be 16 to 128 letters, digits, '-' or '_'";
    }
    return false;
  }
  const auto parse_body = [](const net::Request& req, json::Value& out,
                             std::string& parse_error) {
    json::Limits limits;
    limits.max_bytes = 4ull * 1024 * 1024;
    json::ParseError err;
    auto parsed = json::parse(req.body, limits, &err);
    if (!parsed.has_value()) {
      parse_error = err.message;
      return false;
    }
    out = std::move(*parsed);
    return true;
  };
  impl_->server.route("POST", "/sdk/v1/network/hello",
                      [this, parse_body](const net::Request& req) {
    json::Value body;
    std::string parse_error;
    if (!parse_body(req, body, parse_error)) {
      return net::Response::error(400, "hello body is not valid JSON: " + parse_error);
    }
    return reply(receive_hello(body));
  });
  impl_->server.route("POST", "/sdk/v1/network",
                      [this, parse_body](const net::Request& req) {
    json::Value body;
    std::string parse_error;
    if (!parse_body(req, body, parse_error)) {
      return net::Response::error(400, "batch body is not valid JSON: " + parse_error);
    }
    return reply(receive_events(body));
  });
  if (!impl_->server.listen(port, error)) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = std::chrono::steady_clock::now();
  }
  impl_->running = true;
  impl_->thread = std::thread([this] { impl_->server.serve(impl_->cancel.token()); });
  return true;
}

void NetworkInspect::stop() {
  if (!impl_ || !impl_->running) return;
  impl_->cancel.cancel();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->running = false;
}

bool NetworkInspect::running() const { return impl_ && impl_->running; }

std::uint16_t NetworkInspect::port() const { return impl_->server.port(); }

const std::string& NetworkInspect::token() const { return impl_->server.token(); }

std::string NetworkInspect::adb_reverse_command() const {
  const auto p = std::to_string(port());
  return "adb reverse tcp:" + p + " tcp:" + p;
}

IngestResult NetworkInspect::receive_hello(const json::Value& body) {
  IngestResult result;
  if (!body.is_object()) {
    result.http_status = 400;
    result.error = "hello must be a JSON object";
    return result;
  }
  NetworkHello h;
  const std::size_t n = limits_.max_string_length;
  h.app_identifier = capped(body, "app_identifier", n);
  h.app_version = capped(body, "app_version", n);
  h.build_configuration = capped(body, "build_configuration", n);
  h.platform = capped(body, "platform", n);
  h.device = capped(body, "device", n);
  h.sdk_version = capped(body, "sdk_version", n);
  const json::Value* detail = body.find("detail");
  h.detail = detail != nullptr && detail->as_bool(false);
  if (h.sdk_version.empty()) {
    // Without it, a report could not say which client produced it.
    result.http_status = 400;
    result.error = "hello must name its sdk_version";
    return result;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  hello_ = std::move(h);
  // A new runtime starts its own numbering. Requests already received stay:
  // an app that reloads mid-session has not taken back what it sent.
  next_sequence_ = 1;
  result.accepted = true;
  result.next_sequence = next_sequence_;
  return result;
}

IngestResult NetworkInspect::receive_events(const json::Value& body) {
  IngestResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  result.next_sequence = next_sequence_;
  if (!hello_.has_value()) {
    // Most often DevX restarted while the app kept running. The SDK answers
    // this by saying hello again.
    result.http_status = 409;
    result.error = "say hello first";
    return result;
  }
  const json::Value* events = body.find("events");
  if (!body.is_object() || events == nullptr || !events->is_array()) {
    result.http_status = 400;
    result.error = "a batch is {\"sequence\": n, \"events\": [...]}";
    return result;
  }
  if (events->items().size() > limits_.max_events_per_batch) {
    result.http_status = 413;
    result.error = "too many events in one batch";
    return result;
  }
  const json::Value* seq = body.find("sequence");
  if (seq != nullptr && seq->is_number()) {
    const std::int64_t s = seq->as_int();
    if (s < next_sequence_) {
      // A batch whose reply was lost and that was sent again. Applying it
      // twice would duplicate every request in it.
      result.accepted = true;
      return result;
    }
    if (s > next_sequence_) missing_batches_ += s - next_sequence_;
    next_sequence_ = s + 1;
  }
  const json::Value* dropped = body.find("dropped_by_app");
  if (dropped != nullptr && dropped->is_number() && dropped->as_int() > 0) {
    dropped_by_app_ += dropped->as_int();
  }
  for (const json::Value& e : events->items()) {
    const std::string& method = text_of(e, "method");
    const json::Value* params = e.find("params");
    if (!is_known_event(method) || params == nullptr || !params->is_object()) {
      result.markers_rejected++;
      continue;
    }
    if (method == "DevX.responseBody") {
      const std::string& id = text_of(*params, "requestId");
      const json::Value* text = params->find("body");
      if (text != nullptr && text->is_string()) {
        const json::Value* b64 = params->find("base64Encoded");
        assembler_.set_response_body(id, text->as_string(),
                                     b64 != nullptr && b64->as_bool(false));
      } else {
        const std::string& why = text_of(*params, "unavailable");
        assembler_.set_response_body_unavailable(
            id, why.empty() ? "the app sent no body for this response" : why);
      }
      result.markers_accepted++;
      continue;
    }
    if (method == "Network.requestWillBeSent") {
      if (requests_ >= limits_.max_requests) {
        dropped_here_++;
        continue;
      }
      requests_++;
    }
    assembler_.feed(e);
    result.markers_accepted++;
  }
  rejected_events_ += result.markers_rejected;
  result.accepted = true;
  result.next_sequence = next_sequence_;
  return result;
}

std::optional<NetworkHello> NetworkInspect::hello() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hello_;
}

observe::InspectReport NetworkInspect::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started_)
                        .count();
  // `finish` builds a fresh report from what was fed and changes nothing, so
  // it can run on every poll.
  observe::InspectReport report = assembler_.finish(wall);
  report.debugger_attached = hello_.has_value();
  if (hello_.has_value()) {
    report.app_id = hello_->app_identifier;
    report.device_name = hello_->device;
    report.target_title = "DevX SDK " + hello_->sdk_version;
  }

  observe::SourceStatus net;
  net.name = "network (DevX SDK in the app)";
  net.events = static_cast<std::int64_t>(requests_);
  if (!hello_.has_value()) {
    net.state = observe::SourceState::kUnavailable;
    net.detail =
        "no app has connected. The app calls trackNetwork() with this "
        "endpoint and token; on Android, run the adb reverse line first";
  } else if (requests_ == 0) {
    net.state = observe::SourceState::kRanSawNothing;
    net.detail =
        "the app connected and reported no request yet. This covers "
        "JavaScript fetch and XMLHttpRequest only: a WebView, a native "
        "networking module or the image loader never appears here";
  } else {
    net.state = observe::SourceState::kAttached;
    net.detail = hello_->detail
        ? "the app's own fetch and XMLHttpRequest, with headers and bodies"
        : "the app's own fetch and XMLHttpRequest; the app sends no headers "
          "or bodies (trackNetwork was called with detail: false)";
  }
  report.sources = {net};

  report.caveats = {
      "These come from the DevX SDK inside the app, which wraps its fetch and "
      "XMLHttpRequest. Only JavaScript requests appear: a WebView (most "
      "SSO and payment flows), a native networking module and the image "
      "loader do not. An empty list does not mean the app made no requests.",
      "Requests made before the app connected to DevX are not here: the SDK "
      "records only while DevX is listening.",
      "Headers and bodies are the app's data verbatim, Authorization headers "
      "included. Keep trackNetwork() out of a build that goes to the store.",
      "Times are the app's own clock, measured around fetch and "
      "XMLHttpRequest in JavaScript: they include time the JavaScript thread "
      "was busy, so they are what the app experienced, not the network "
      "alone.",
  };
  if (missing_batches_ > 0) {
    report.caveats.push_back(std::to_string(missing_batches_) +
                             " batch(es) the app sent never arrived, so "
                             "some requests are missing.");
  }
  if (dropped_by_app_ > 0) {
    report.caveats.push_back(std::to_string(dropped_by_app_) +
                             " event(s) were dropped inside the app because "
                             "its buffer was full.");
  }
  if (dropped_here_ > 0) {
    report.caveats.push_back(std::to_string(dropped_here_) +
                             " request(s) arrived after this session held " +
                             std::to_string(limits_.max_requests) +
                             " and were not kept. Stop and start again.");
  }
  if (rejected_events_ > 0) {
    report.caveats.push_back(std::to_string(rejected_events_) +
                             " event(s) were refused as malformed.");
  }
  return report;
}

}  // namespace mpi::sdk
