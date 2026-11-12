#include <sstream>
#include <string>

#include "core/sdk/marker_ingest.hpp"
#include "core/sdk/network_inspect.hpp"
#include "core/sdk/sdk_bridge.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

json::Value parse_or_die(const std::string& text) {
  json::ParseError err;
  auto v = json::parse(text, &err);
  return v.has_value() ? *v : json::Value::object();
}

json::Value handshake_body() {
  return parse_or_die(R"({
    "app_identifier": "io.example.app",
    "sdk_version": "0.1.0",
    "app_version": "4.2.0",
    "build_number": "1187",
    "build_configuration": "Staging",
    "js_engine": "hermes",
    "js_engine_version": "0.12.0",
    "react_native_version": "0.74.3",
    "architecture": "fabric",
    "js_bundle_id": "bundle-abc123",
    "ota_update_id": "ota-77",
    "native_build_id": "9f1c2e",
    "js_dev_mode": false,
    "native_debuggable": true,
    "app_clock_ns": 1234567890
  })");
}

std::string batch(int sequence, const std::string& markers,
                  const std::string& extra = "") {
  std::ostringstream os;
  os << "{\"sequence\": " << sequence;
  if (!extra.empty()) os << ", " << extra;
  os << ", \"markers\": [" << markers << "]}";
  return os.str();
}

const char* kScreenMount =
    R"({"kind": "screen_mount", "timestamp_ns": 1000, "screen": "Checkout"})";

bool mentions(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v) {
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

const model::BuildFact* fact_for(const std::vector<model::BuildFact>& facts,
                                 const std::string& key) {
  for (const auto& f : facts) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(sdk_handshake_becomes_runtime_build_facts, {"A19", "C05", "C16", "G15"}) {
  sdk::MarkerIngest ingest;
  const auto result = ingest.receive_handshake(handshake_body());
  MPI_CHECK(result.accepted);

  const auto facts = ingest.build_facts();
  // The build configuration is the app's own name for it, not one of three
  // hard-coded choices (spec C05).
  const auto* config = fact_for(facts, "build.configuration");
  MPI_CHECK(config != nullptr);
  if (config != nullptr) {
    MPI_CHECK_EQ(config->value, std::string("Staging"));
    // Reported at runtime, which is weaker than a build manifest: the source
    // travels with the fact so a reader can weigh it.
    MPI_CHECK(config->source == model::FactSource::kRuntimeSdk);
  }
  // OTA identity, which no device provider can see (C16).
  MPI_CHECK(fact_for(facts, "js.ota_update_id") != nullptr);
  MPI_CHECK(fact_for(facts, "js.bundle_id") != nullptr);

  const auto* dev = fact_for(facts, "js.__DEV__");
  MPI_CHECK(dev != nullptr);
  if (dev != nullptr) MPI_CHECK(dev->boolean_value == model::Tri::kFalse);
}

MPI_TEST(sdk_refuses_a_handshake_that_does_not_identify_its_producer, {"A19"}) {
  sdk::MarkerIngest ingest;
  const auto result = ingest.receive_handshake(parse_or_die(R"({"app_version":"1"})"));
  MPI_CHECK(!result.accepted);
  MPI_CHECK_EQ(result.http_status, 400);
  MPI_CHECK(result.error.find("sdk_version") != std::string::npos);
}

MPI_TEST(sdk_markers_need_a_handshake_first, {"A19", "J06"}) {
  sdk::MarkerIngest ingest;
  const auto result = ingest.receive_markers(parse_or_die(batch(1, kScreenMount)));
  MPI_CHECK(!result.accepted);
  MPI_CHECK_EQ(result.http_status, 409);
  // A marker whose origin is unknown is not evidence about any build.
  MPI_CHECK(result.error.find("attributed to a build") != std::string::npos);
}

MPI_TEST(sdk_accepts_the_marker_kinds_the_spec_names, {"G07", "G08", "section-11"}) {
  for (const char* kind :
       {"screen_mount", "screen_unmount", "navigation_begin", "navigation_end",
        "navigation_cancel", "interaction_begin", "interaction_end",
        "async_span_begin", "async_span_end"}) {
    MPI_CHECK_MSG(sdk::is_known_marker_kind(kind),
                  std::string("spec section 11 names ") + kind);
  }
  // A cancelled navigation is its own kind: "cancelled" and "never finished"
  // are different facts (G07).
  MPI_CHECK(sdk::is_known_marker_kind("navigation_cancel"));
  MPI_CHECK(!sdk::is_known_marker_kind("screen_maybe_mounted"));
}

MPI_TEST(sdk_records_a_lost_batch_as_a_gap_not_as_silence, {"J07", "E13"}) {
  sdk::MarkerIngest ingest;
  ingest.receive_handshake(handshake_body());
  MPI_CHECK(ingest.receive_markers(parse_or_die(batch(1, kScreenMount))).accepted);

  // Batch 2 never arrives; the app sends 3.
  const auto later = ingest.receive_markers(parse_or_die(
      batch(3, R"({"kind": "screen_unmount", "timestamp_ns": 2000, "screen": "Checkout"})")));
  MPI_CHECK(later.accepted);
  MPI_CHECK_EQ(ingest.missing_batches(), std::int64_t{1});
  MPI_CHECK(mentions(ingest.notes(), "never arrived"));
  MPI_CHECK(mentions(ingest.notes(), "not an absence of activity"));

  // And the capability carries it, so a rule reading these markers can see
  // that part of the stream is missing.
  const auto cap = ingest.capability();
  MPI_CHECK(cap.status == model::CapabilityStatus::kAvailable);
  MPI_CHECK(mentions(cap.limitations, "never arrived"));
}

MPI_TEST(sdk_does_not_store_a_retried_batch_twice, {"J07"}) {
  sdk::MarkerIngest ingest;
  ingest.receive_handshake(handshake_body());
  MPI_CHECK(ingest.receive_markers(parse_or_die(batch(1, kScreenMount))).accepted);
  // The client lost the response and retried the same batch.
  const auto retry = ingest.receive_markers(parse_or_die(batch(1, kScreenMount)));
  MPI_CHECK(retry.accepted);
  MPI_CHECK_EQ(ingest.duplicate_batches(), std::int64_t{1});
  MPI_CHECK_EQ(ingest.take_markers().size(), std::size_t{1});
  MPI_CHECK(mentions(retry.warnings, "not stored twice"));
}

MPI_TEST(sdk_counts_what_the_app_dropped_before_sending, {"J07", "E13"}) {
  sdk::MarkerIngest ingest;
  ingest.receive_handshake(handshake_body());
  ingest.receive_markers(
      parse_or_die(batch(1, kScreenMount, "\"dropped_by_app\": 17")));
  MPI_CHECK_EQ(ingest.markers_dropped_by_app(), std::int64_t{17});
  MPI_CHECK(mentions(ingest.notes(), "dropping 17 marker(s) from its own buffer"));
}

MPI_TEST(sdk_rejects_malformed_and_unknown_markers, {"J01", "D19"}) {
  sdk::MarkerIngest ingest;
  ingest.receive_handshake(handshake_body());
  const auto result = ingest.receive_markers(parse_or_die(batch(
      1,
      R"({"kind": "screen_mount", "timestamp_ns": 1000, "screen": "A"},)"
      R"("not an object",)"
      R"({"kind": "screen_mount"},)"                       // no timestamp
      R"({"timestamp_ns": 5},)"                            // no kind
      R"({"kind": "telepathy", "timestamp_ns": 6},)"       // unknown kind
      R"({"kind": "screen_mount", "timestamp_ns": -3})")));  // impossible time
  MPI_CHECK(result.accepted);
  MPI_CHECK_EQ(result.markers_accepted, std::int64_t{1});
  MPI_CHECK_EQ(result.markers_rejected, std::int64_t{5});
  MPI_CHECK(mentions(result.warnings, "unknown marker kind 'telepathy'"));
  // Rejections are counted, not silently absorbed.
  MPI_CHECK_EQ(ingest.markers_rejected(), std::int64_t{5});
  MPI_CHECK(mentions(ingest.capability().limitations, "were rejected"));
}

MPI_TEST(sdk_refuses_a_batch_over_the_limit, {"J03", "J07"}) {
  sdk::IngestLimits limits;
  limits.max_markers_per_batch = 2;
  sdk::MarkerIngest ingest(limits);
  ingest.receive_handshake(handshake_body());
  const auto result = ingest.receive_markers(parse_or_die(batch(
      1,
      R"({"kind":"screen_mount","timestamp_ns":1},)"
      R"({"kind":"screen_mount","timestamp_ns":2},)"
      R"({"kind":"screen_mount","timestamp_ns":3})")));
  MPI_CHECK(!result.accepted);
  MPI_CHECK_EQ(result.http_status, 413);
}

MPI_TEST(sdk_applies_backpressure_instead_of_growing_without_limit, {"J07"}) {
  sdk::IngestLimits limits;
  limits.max_markers_per_session = 1;
  sdk::MarkerIngest ingest(limits);
  ingest.receive_handshake(handshake_body());
  MPI_CHECK(ingest.receive_markers(parse_or_die(batch(1, kScreenMount))).accepted);

  const auto refused = ingest.receive_markers(parse_or_die(
      batch(2, R"({"kind":"screen_mount","timestamp_ns":9000,"screen":"B"})")));
  MPI_CHECK(!refused.accepted);
  MPI_CHECK_EQ(refused.http_status, 429);
  MPI_CHECK(refused.retry_after_ms.has_value());
  // And the capture records that its later part has no SDK evidence.
  MPI_CHECK(mentions(ingest.notes(), "no SDK evidence"));
}

MPI_TEST(sdk_notices_a_reload_onto_a_different_bundle, {"G04", "G15", "C16"}) {
  sdk::MarkerIngest ingest;
  ingest.receive_handshake(handshake_body());
  auto second = handshake_body();
  second.set("js_bundle_id", json::Value::string("bundle-def456"));
  MPI_CHECK(ingest.receive_handshake(second).accepted);
  MPI_CHECK(mentions(ingest.notes(), "different JS bundle id"));
  MPI_CHECK(mentions(ingest.notes(), "not interchangeable"));
}

MPI_TEST(sdk_absence_is_reported_as_no_evidence_not_as_a_clean_app,
         {"A19", "H11", "section-11"}) {
  sdk::MarkerIngest ingest;
  const auto cap = ingest.capability();
  MPI_CHECK(cap.status == model::CapabilityStatus::kUnsupported);
  MPI_CHECK(cap.evidence.find("no app ever handshook") != std::string::npos);
  // The reason screen and interaction stay empty is stated, because the
  // alternative -- inferring a screen from a function name -- is forbidden.
  MPI_CHECK(mentions(cap.limitations, "never guessed"));
}

MPI_TEST(sdk_bridge_binds_loopback_and_requires_a_token, {"J06", "section-14"}) {
  sdk::SdkBridge bridge;
  std::string error;
  MPI_CHECK_MSG(bridge.start(0, &error), "bridge did not start: " + error);
  MPI_CHECK(bridge.port() != 0);
  MPI_CHECK(!bridge.token().empty());
  // The command an operator needs for a device to reach it, rather than a
  // network-exposed listener.
  MPI_CHECK(bridge.adb_reverse_command().find("adb reverse tcp:") !=
            std::string::npos);
  bridge.stop();
}

// --- trackNetwork(): Inspect for a release build ------------------------------

namespace {

// What the SDK sends for one GraphQL POST: the events a debugger would have
// received, and the response body it would have asked for.
const char* kGraphqlBatch = R"({"sequence": 1, "events": [
  {"method":"Network.requestWillBeSent","params":{"requestId":"sdk-1",
   "timestamp":10.0,"wallTime":1762905600.5,
   "request":{"method":"POST","url":"https://api.example.com/graphql",
     "headers":{"Content-Type":"application/json","Authorization":"Bearer abc"},
     "postData":"{\"operationName\":\"GetMenu\",\"query\":\"query GetMenu { menu { id } }\"}"}}},
  {"method":"Network.responseReceived","params":{"requestId":"sdk-1","timestamp":10.2,
   "response":{"url":"https://api.example.com/graphql","status":200,
     "mimeType":"application/json","headers":{"content-type":"application/json"}}}},
  {"method":"Network.loadingFinished","params":{"requestId":"sdk-1","timestamp":10.3,
   "encodedDataLength":27}},
  {"method":"DevX.responseBody","params":{"requestId":"sdk-1",
   "body":"{\"data\":{\"menu\":[{\"id\":1}]}}","base64Encoded":false}},
  {"method":"Runtime.evaluate","params":{}}
]})";

}  // namespace

MPI_TEST(sdk_network_reads_like_a_debug_build_inspect, {}) {
  sdk::NetworkInspect listener;
  MPI_CHECK_MSG(listener.receive_events(parse_or_die(kGraphqlBatch)).http_status == 409,
                "a batch before hello is refused, so the SDK says hello again");
  const auto hello = listener.receive_hello(parse_or_die(
      R"({"app_identifier":"com.kfc.app","app_version":"9.1","platform":"android",
          "device":"Pixel 8","sdk_version":"0.2.0","detail":true})"));
  MPI_CHECK(hello.accepted);
  const auto r = listener.receive_events(parse_or_die(kGraphqlBatch));
  MPI_CHECK(r.accepted);
  MPI_CHECK_MSG(r.markers_accepted == 4 && r.markers_rejected == 1,
                "the four network events are taken; a Runtime command is not");

  const auto report = listener.snapshot();
  MPI_CHECK(report.debugger_attached && report.app_id == "com.kfc.app");
  MPI_CHECK(report.network.size() == 1);
  const auto& ex = report.network[0];
  MPI_CHECK(ex.method == "POST" && ex.status.value_or(0) == 200);
  MPI_CHECK_MSG(ex.request_body.value_or("").find("GetMenu") != std::string::npos,
                "the request body arrives as the app sent it");
  MPI_CHECK_MSG(ex.response_body.value_or("") == R"({"data":{"menu":[{"id":1}]}})",
                "and so does the response body");
  MPI_CHECK_MSG(ex.request_headers.count("Authorization") == 1,
                "headers are kept: the app chose to send them");
  MPI_CHECK(report.sources.size() == 1 &&
            report.sources[0].state == observe::SourceState::kAttached);
  MPI_CHECK(mentions(report.caveats, "malformed"));

  // The same batch again -- its reply was lost and the SDK resent it.
  listener.receive_events(parse_or_die(kGraphqlBatch));
  MPI_CHECK_MSG(listener.snapshot().network.size() == 1,
                "a resent batch is not applied twice");
  // A gap in the numbering is a batch that never arrived.
  listener.receive_events(parse_or_die(R"({"sequence": 4, "events": []})"));
  MPI_CHECK(mentions(listener.snapshot().caveats, "never arrived"));
}

MPI_TEST(sdk_network_before_any_app_says_so, {}) {
  sdk::NetworkInspect listener;
  const auto report = listener.snapshot();
  MPI_CHECK(!report.debugger_attached);
  MPI_CHECK(report.sources.size() == 1 &&
            report.sources[0].state == observe::SourceState::kUnavailable);
  MPI_CHECK(listener.receive_hello(parse_or_die(R"({"app_identifier":"x"})")).http_status == 400);
}

MPI_TEST(sdk_network_keeps_a_token_a_build_has_built_in, {"section-14"}) {
  sdk::NetworkInspect weak;
  std::string error;
  MPI_CHECK_MSG(!weak.start(0, "short", &error), "a weak token is refused");
  MPI_CHECK(!weak.running());
  sdk::NetworkInspect fixed;
  MPI_CHECK_MSG(fixed.start(0, "devx-0123456789abcdef", &error), error);
  MPI_CHECK(fixed.token() == "devx-0123456789abcdef");
  fixed.stop();
}
