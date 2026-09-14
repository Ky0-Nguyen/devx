#include "core/sdk/sdk_bridge.hpp"

#include <thread>

#include "core/util/json.hpp"

namespace mpi::sdk {
namespace {

net::Response reply(const IngestResult& result) {
  json::Value body = json::Value::object();
  body.set("accepted", json::Value::boolean(result.accepted));
  body.set("markers_accepted",
           json::Value::number(static_cast<double>(result.markers_accepted)));
  body.set("markers_rejected",
           json::Value::number(static_cast<double>(result.markers_rejected)));
  // The client needs this to resynchronise after a lost response instead of
  // resending blindly.
  body.set("next_sequence",
           json::Value::number(static_cast<double>(result.next_sequence)));
  if (!result.error.empty()) body.set("error", json::Value::string(result.error));
  if (!result.warnings.empty()) {
    json::Value warnings = json::Value::array();
    for (const auto& w : result.warnings) warnings.push_back(json::Value::string(w));
    body.set("warnings", std::move(warnings));
  }
  net::Response response = net::Response::json(body.dump(), result.http_status);
  if (result.retry_after_ms.has_value()) {
    // Backpressure the client can act on, rather than a bare refusal (J07).
    response.headers.push_back(
        {"Retry-After-Ms", std::to_string(*result.retry_after_ms)});
  }
  return response;
}

}  // namespace

struct SdkBridge::Impl {
  net::HttpServer server;
  CancellationSource cancel;
  std::thread thread;
  bool running = false;
};

SdkBridge::SdkBridge(IngestLimits limits)
    : ingest_(limits), impl_(std::make_unique<Impl>()) {}

SdkBridge::~SdkBridge() { stop(); }

bool SdkBridge::start(std::uint16_t port, std::string* error) {
  const auto parse_body = [](const net::Request& req, json::Value& out,
                             std::string& parse_error) {
    json::Limits limits;
    // A marker batch is small by contract; a body larger than this is a
    // client bug or an attempt, and either way it is refused before parsing.
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

  impl_->server.route("POST", "/sdk/v1/hello", [this, parse_body](const net::Request& req) {
    json::Value body;
    std::string parse_error;
    if (!parse_body(req, body, parse_error)) {
      return net::Response::error(400, "handshake body is not valid JSON: " + parse_error);
    }
    return reply(ingest_.receive_handshake(body));
  });

  impl_->server.route("POST", "/sdk/v1/markers", [this, parse_body](const net::Request& req) {
    json::Value body;
    std::string parse_error;
    if (!parse_body(req, body, parse_error)) {
      return net::Response::error(400, "batch body is not valid JSON: " + parse_error);
    }
    return reply(ingest_.receive_markers(body));
  });

  if (!impl_->server.listen(port, error)) return false;
  impl_->running = true;
  impl_->thread = std::thread([this] {
    impl_->server.serve(impl_->cancel.token());
  });
  return true;
}

void SdkBridge::stop() {
  if (!impl_ || !impl_->running) return;
  impl_->cancel.cancel();
  // The accept loop wakes on its own poll timeout, so joining is enough; no
  // socket is forced closed underneath a handler mid-request.
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->running = false;
}

std::uint16_t SdkBridge::port() const { return impl_->server.port(); }

const std::string& SdkBridge::token() const { return impl_->server.token(); }

std::string SdkBridge::adb_reverse_command() const {
  const auto p = std::to_string(port());
  return "adb reverse tcp:" + p + " tcp:" + p;
}

}  // namespace mpi::sdk
