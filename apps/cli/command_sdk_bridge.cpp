// `mpi sdk-bridge` -- accept SDK markers without recording.
//
// Two uses, both real.
//
// An app developer integrating the SDK needs to know whether their markers
// arrive at all, and what the host makes of them, before wiring a capture
// around it. Running the bridge on its own answers that in one command: it
// prints the endpoint and the `adb reverse` line, then reports what it
// received -- including what it decided was missing.
//
// And it makes the SDK's own end-to-end test possible: the JavaScript client
// can be driven against the real C++ endpoint rather than a mock of it, which
// is the only way the sequence and backpressure behaviour gets exercised as a
// pair.
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "apps/cli/cli.hpp"
#include "core/sdk/sdk_bridge.hpp"
#include "core/util/json.hpp"
#include "core/util/time.hpp"

namespace mpi::cli {

ExitCode cmd_sdk_bridge(const Invocation& inv) {
  sdk::IngestLimits limits;
  if (inv.has_flag("max-markers")) {
    const int n = std::atoi(inv.flag("max-markers").c_str());
    if (n <= 0) {
      std::cerr << "error: --max-markers must be a positive integer\n";
      return ExitCode::kUsage;
    }
    limits.max_markers_per_session = static_cast<std::size_t>(n);
  }

  std::uint16_t port = 0;
  if (inv.has_flag("sdk-port")) {
    const int p = std::atoi(inv.flag("sdk-port").c_str());
    if (p < 1 || p > 65535) {
      std::cerr << "error: --sdk-port must be between 1 and 65535\n";
      return ExitCode::kUsage;
    }
    port = static_cast<std::uint16_t>(p);
  }

  sdk::SdkBridge bridge(limits);
  std::string error;
  if (!bridge.start(port, &error)) {
    std::cerr << "error: cannot start the SDK endpoint: " << error << "\n";
    return ExitCode::kCollectionError;
  }

  // The first line is machine-readable on purpose: a test or a script reads
  // the port and token from it without parsing prose.
  std::cout << "endpoint http://127.0.0.1:" << bridge.port() << " token "
            << bridge.token() << std::endl;
  std::cerr << "SDK endpoint listening on 127.0.0.1:" << bridge.port()
            << " (loopback only, token required)\n"
            << "  for an Android device: " << bridge.adb_reverse_command()
            << "\n"
            << "  for the iOS simulator: loopback is already shared\n";

  const int seconds = inv.has_flag("duration-s")
                          ? std::atoi(inv.flag("duration-s").c_str())
                          : 0;
  if (seconds > 0) {
    std::cerr << "  stopping after " << seconds << "s\n";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      if (inv.global.cancel.cancelled()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } else {
    std::cerr << "  reading until stdin closes (Ctrl-D), or Ctrl-C\n";
    std::string line;
    while (!inv.global.cancel.cancelled() && std::getline(std::cin, line)) {
      if (line == "stop") break;
    }
  }

  bridge.stop();

  const auto& ingest = bridge.ingest();
  const auto markers = const_cast<sdk::MarkerIngest&>(ingest).take_markers();
  const auto capability = ingest.capability();

  json::Value summary = json::Value::object();
  summary.set("markers", json::Value::number(static_cast<double>(markers.size())));
  summary.set("missing_batches",
              json::Value::number(static_cast<double>(ingest.missing_batches())));
  summary.set("duplicate_batches",
              json::Value::number(static_cast<double>(ingest.duplicate_batches())));
  summary.set("markers_dropped_by_app",
              json::Value::number(
                  static_cast<double>(ingest.markers_dropped_by_app())));
  summary.set("markers_rejected",
              json::Value::number(static_cast<double>(ingest.markers_rejected())));
  summary.set("capability_status",
              json::Value::string(model::to_string(capability.status)));
  summary.set("capability_evidence", json::Value::string(capability.evidence));
  json::Value handshook = json::Value::object();
  if (const auto h = ingest.handshake()) {
    handshook.set("sdk_version", json::Value::string(h->sdk_version));
    handshook.set("app_identifier", json::Value::string(h->app_identifier));
    handshook.set("js_bundle_id", json::Value::string(h->js_bundle_id));
    handshook.set("clock_domain", json::Value::string(h->clock_domain));
  }
  summary.set("handshake", std::move(handshook));
  json::Value notes = json::Value::array();
  for (const auto& n : ingest.notes()) notes.push_back(json::Value::string(n));
  summary.set("notes", std::move(notes));
  json::Value kinds = json::Value::object();
  for (const auto& m : markers) {
    const auto* existing = kinds.find(m.kind);
    const double count = existing == nullptr ? 0.0 : existing->as_double();
    kinds.set(m.kind, json::Value::number(count + 1.0));
  }
  summary.set("kinds", std::move(kinds));

  const std::string out_path = inv.flag("out");
  if (!out_path.empty()) {
    // The markers themselves, so an integration can be inspected rather than
    // only counted.
    json::Value doc = json::Value::object();
    doc.set("schema_version", json::Value::string("2.0"));
    doc.set("summary", summary);
    json::Value array = json::Value::array();
    for (const auto& m : markers) array.push_back(m.to_json());
    doc.set("markers", std::move(array));
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot write " << out_path << "\n";
      return ExitCode::kCollectionError;
    }
    const auto text = doc.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    std::cerr << "wrote " << out_path << "\n";
  }

  std::cout << summary.dump(2) << std::endl;

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  // An app that never connected is not a success: the operator asked for
  // markers and got none.
  if (!ingest.handshake().has_value()) {
    std::cerr << "note: no app handshook with this endpoint, so nothing was "
                 "received. Check the endpoint, the token, and `adb reverse` "
                 "on Android.\n";
    return ExitCode::kInconclusive;
  }
  return ExitCode::kOk;
}

}  // namespace mpi::cli
