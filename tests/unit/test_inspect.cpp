// The inspect assembler, the websocket codec, and the handshake hash.
//
// The fixture under fixtures/cdp is a **real** recorded session from a React
// Native app, not a constructed one. That matters here more than usual: the
// whole feature rests on reading a protocol as an actual runtime emits it,
// and a test written against the shapes the parser expects would pass while
// the parser silently dropped everything real.
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/net/websocket_client.hpp"
#include "core/observe/inspect.hpp"
#include "adapters/rn/inspector.hpp"
#include "core/observe/screenshot.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

std::string fixture_dir() {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  return dir != nullptr ? std::string(dir) : std::string("fixtures");
}

/// Feeds the recorded session, skipping the `//` provenance header.
mpi::observe::InspectReport replay_fixture(std::int64_t* fed) {
  std::ifstream in(fixture_dir() + "/cdp/recorded-hutbot-startup.jsonl");
  mpi::observe::InspectAssembler assembler;
  std::string line;
  std::int64_t count = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line.rfind("//", 0) == 0) continue;
    mpi::json::ParseError err;
    auto doc = mpi::json::parse(line, &err);
    MPI_CHECK_MSG(doc.has_value(), "the recorded fixture must parse");
    assembler.feed(*doc);
    count++;
  }
  if (fed != nullptr) *fed = count;
  return assembler.finish(30000);
}

}  // namespace

MPI_TEST(recorded_session_is_read_as_recorded, {}) {
  std::int64_t fed = 0;
  const auto report = replay_fixture(&fed);
  MPI_CHECK_MSG(fed >= 20, "the fixture should carry the whole session");

  // The session really did contain three complete HTTP exchanges.
  MPI_CHECK_MSG(report.network.size() == 3,
                "three requests were recorded, and three must come back");
  for (const auto& e : report.network) {
    MPI_CHECK_MSG(!e.url.empty(), "every exchange keeps its URL");
    MPI_CHECK_MSG(!e.method.empty(), "and its method");
    MPI_CHECK_MSG(e.status.has_value(),
                  "all three got a response in the recording");
    MPI_CHECK_MSG(!e.incomplete,
                  "and none was in flight when the recording ended");
    const auto ms = e.duration_ms();
    MPI_CHECK_MSG(ms.has_value(), "so each has a duration");
    MPI_CHECK_MSG(*ms >= 0.0, "and it is not negative");
  }

  MPI_CHECK_MSG(report.console.size() >= 10,
                "the console entries are not dropped");
  // Ordering is by timestamp, and the two domains use different units --
  // getting that wrong once put console lines in 1970 beside requests in the
  // present, which sorted them all to the front.
  for (std::size_t i = 1; i < report.console.size(); i++) {
    MPI_CHECK_MSG(report.console[i - 1].timestamp_ns <=
                      report.console[i].timestamp_ns,
                  "console entries come out in time order");
  }
  for (const auto& c : report.console) {
    MPI_CHECK_MSG(c.timestamp_ns > 1'000'000'000'000'000'000LL,
                  "every console timestamp is a plausible epoch time, not a "
                  "seconds value read as nanoseconds");
  }
}

MPI_TEST(a_source_that_saw_nothing_is_not_a_source_that_failed, {}) {
  mpi::observe::InspectAssembler assembler;
  const auto empty = assembler.finish(5000);
  MPI_CHECK_MSG(empty.network.empty(), "nothing was fed, so nothing is there");
  bool found_network_source = false;
  for (const auto& s : empty.sources) {
    if (s.name.find("network") == std::string::npos) continue;
    found_network_source = true;
    MPI_CHECK_MSG(s.state == mpi::observe::SourceState::kRanSawNothing,
                  "an attached source with no events says it saw nothing");
    MPI_CHECK_MSG(s.state != mpi::observe::SourceState::kUnavailable,
                  "which is a different answer from being unreachable");
    // It must name what the gap actually is. "WebView" earns its place here
    // by measurement: driving a React Native SSO login -- typing into the
    // form and submitting it -- produced no entry at all, because a
    // WebView's requests are native and never reach the JS inspector.
    MPI_CHECK_MSG(s.detail.find("WebView") != std::string::npos,
                  "the detail names WebView, which is where most SSO and "
                  "payment traffic goes and the most common reason this list "
                  "looks empty while the app is plainly busy");
    MPI_CHECK_MSG(s.detail.find("native") != std::string::npos,
                  "and native code generally, so an empty list is not read "
                  "as 'the app made no requests'");
    MPI_CHECK_MSG(s.detail.find("fetch/XHR") != std::string::npos,
                  "and says what it does cover, rather than only what it "
                  "does not");
  }
  MPI_CHECK(found_network_source);
  MPI_CHECK_MSG(empty.caveats.size() >= 4,
                "the limits travel with the report, not just the docs");
}

MPI_TEST(an_unfinished_request_is_not_an_instant_one, {}) {
  mpi::observe::InspectAssembler assembler;
  mpi::json::ParseError err;
  auto start = mpi::json::parse(
      R"({"method":"Network.requestWillBeSent","params":{"requestId":"r1",
         "timestamp":100.0,"request":{"method":"GET","url":"https://x/y"}}})",
      &err);
  MPI_CHECK(start.has_value());
  assembler.feed(*start);
  const auto report = assembler.finish(1000);
  MPI_CHECK(report.network.size() == 1);
  const auto& ex = report.network.front();
  MPI_CHECK_MSG(ex.incomplete, "a request with no response is marked");
  MPI_CHECK_MSG(!ex.duration_ms().has_value(),
                "and has no duration, rather than a zero one implying an "
                "instant reply");
  MPI_CHECK_MSG(!ex.status.has_value(),
                "and no status, rather than 0, which is a status");
}

MPI_TEST(a_response_without_a_start_is_kept_but_not_timed, {}) {
  // Attaching mid-flight is normal: the request began before we were there.
  mpi::observe::InspectAssembler assembler;
  mpi::json::ParseError err;
  auto resp = mpi::json::parse(
      R"({"method":"Network.responseReceived","params":{"requestId":"r9",
         "response":{"status":200,"url":"https://a/b","mimeType":"application/json"}}})",
      &err);
  MPI_CHECK(resp.has_value());
  assembler.feed(*resp);
  const auto report = assembler.finish(1000);
  MPI_CHECK_MSG(report.network.size() == 1,
                "the response is real and is kept");
  MPI_CHECK_MSG(report.network.front().status.value_or(0) == 200,
                "with its status");
  MPI_CHECK_MSG(!report.network.front().duration_ms().has_value(),
                "and no duration, because the start was never seen");
}

MPI_TEST(terminal_colouring_is_removed_not_rendered, {}) {
  using A = mpi::observe::InspectAssembler;
  // Exactly what React Native emits around its own warnings.
  const std::string coloured =
      "\x1b[48;2;253;247;231m\x1b[30m\x1b[1mNOTE: \x1b[22mmessage";
  const std::string clean = A::strip_ansi(coloured);
  MPI_CHECK_MSG(clean == "NOTE: message", "the text survives, the escapes go");
  MPI_CHECK_MSG(clean.find('\x1b') == std::string::npos,
                "no escape byte reaches a report or a terminal");
  // A lone ESC with no CSI must not eat the rest of the line.
  MPI_CHECK(A::strip_ansi("a\x1b" "b") == "ab");
  MPI_CHECK(A::strip_ansi("") == "");
  MPI_CHECK(A::strip_ansi("plain") == "plain");
  // An unterminated CSI is the pathological case: it must not loop or crash.
  MPI_CHECK(A::strip_ansi("x\x1b[38;2;1;2;3").size() <= 1);
}

MPI_TEST(console_arguments_never_silently_vanish, {}) {
  using A = mpi::observe::InspectAssembler;
  mpi::json::ParseError err;
  auto args = mpi::json::parse(
      R"([{"type":"string","value":"hello"},
          {"type":"number","value":42},
          {"type":"boolean","value":true},
          {"type":"object","description":"Object"},
          {"type":"function"}])",
      &err);
  MPI_CHECK(args.has_value());
  const std::string joined = A::join_console_args(*args);
  MPI_CHECK_MSG(joined.find("hello") != std::string::npos, "strings render");
  MPI_CHECK_MSG(joined.find("42") != std::string::npos, "numbers render");
  MPI_CHECK_MSG(joined.find("true") != std::string::npos, "booleans render");
  MPI_CHECK_MSG(joined.find("Object") != std::string::npos,
                "an object uses the runtime's own description");
  MPI_CHECK_MSG(joined.find("<function>") != std::string::npos,
                "and an argument with nothing renderable is named rather than "
                "dropped, so a log line does not lose a value");
}

MPI_TEST(a_redux_action_is_recognised_but_labelled_a_guess, {}) {
  using A = mpi::observe::InspectAssembler;
  // redux-logger's printed forms.
  const auto one = A::redux_action_type("action user/login @ 11:02:03.456");
  MPI_CHECK(one.has_value() && *one == "user/login");
  const auto two = A::redux_action_type("action  cart/addItem");
  MPI_CHECK(two.has_value() && *two == "cart/addItem");
  // Prose that merely begins with the word is not an action.
  MPI_CHECK(!A::redux_action_type("actions are dispatched here").has_value());
  MPI_CHECK(!A::redux_action_type("action ").has_value());
  MPI_CHECK(!A::redux_action_type("action {type: 'x'}").has_value());
  MPI_CHECK(!A::redux_action_type("dispatched action user/login").has_value());

  // And in the report it is never presented as something the store said.
  mpi::observe::InspectAssembler assembler;
  mpi::json::ParseError err;
  auto ev = mpi::json::parse(
      R"({"method":"Runtime.consoleAPICalled","params":{"type":"log",
         "timestamp":1700000000000.0,
         "args":[{"type":"string","value":"action user/login @ 1"}]}})",
      &err);
  MPI_CHECK(ev.has_value());
  assembler.feed(*ev);
  const auto json = assembler.finish(1).to_json();
  const std::string dumped = json.dump();
  MPI_CHECK_MSG(dumped.find("inferred_redux_action_type") != std::string::npos,
                "the field name says it is inferred");
  MPI_CHECK_MSG(dumped.find("the store did not report it") != std::string::npos,
                "and the basis is stated alongside it");
}

MPI_TEST(multi_line_console_output_stays_one_row, {}) {
  using A = mpi::observe::InspectAssembler;
  const std::string pretty = "config {\n  \"a\": 1,\n  \"b\": 2\n}";
  const std::string one = A::single_line(pretty, 200);
  MPI_CHECK_MSG(one.find('\n') == std::string::npos, "no newline survives");
  MPI_CHECK_MSG(one.find(" / ") != std::string::npos,
                "and the line breaks are visible, so three lines do not read "
                "as one sentence");
  MPI_CHECK(A::single_line("short", 200) == "short");
  MPI_CHECK(A::single_line("abcdefghij", 6) == "abcd..");
}

MPI_TEST(unknown_protocol_messages_are_counted_not_guessed, {}) {
  mpi::observe::InspectAssembler assembler;
  mpi::json::ParseError err;
  auto weird = mpi::json::parse(
      R"({"method":"Fictional.somethingHappened","params":{"x":1}})", &err);
  MPI_CHECK(weird.has_value());
  assembler.feed(*weird);
  MPI_CHECK_MSG(assembler.unrecognised() == 1, "it is counted");
  // A command reply is not an event and is not an unknown message.
  auto reply = mpi::json::parse(R"({"id":7,"result":{}})", &err);
  MPI_CHECK(reply.has_value());
  assembler.feed(*reply);
  MPI_CHECK_MSG(assembler.unrecognised() == 1,
                "a command reply is not counted as an unrecognised event");
}

MPI_TEST(websocket_handshake_hash_matches_the_standard, {}) {
  using W = mpi::net::WebSocketClient;
  // The example from RFC 6455 section 1.3. If this is wrong, every connection
  // is refused -- or worse, accepted against something that is not a
  // websocket.
  MPI_CHECK(W::handshake_accept("dGhlIHNhbXBsZSBub25jZQ==") ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

  // SHA-1 known answers, so a failure here points at the hash rather than at
  // the base64 or the concatenation.
  unsigned char digest[20];
  W::sha1(reinterpret_cast<const unsigned char*>(""), 0, digest);
  MPI_CHECK(W::base64(digest, 20) == "2jmj7l5rSw0yVb/vlWAYkK/YBwk=");
  const std::string abc = "abc";
  W::sha1(reinterpret_cast<const unsigned char*>(abc.data()), abc.size(), digest);
  MPI_CHECK(W::base64(digest, 20) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
  // A message that crosses the 64-byte block boundary and forces a second
  // padding block, which is where a length-in-bits mistake shows up.
  const std::string long_input(1000, 'a');
  W::sha1(reinterpret_cast<const unsigned char*>(long_input.data()),
          long_input.size(), digest);
  MPI_CHECK(W::base64(digest, 20) == "KR6abGaZSUm1e6XmUDYemPw2sbo=");
}

MPI_TEST(websocket_frames_are_encoded_to_the_wire_format, {}) {
  using W = mpi::net::WebSocketClient;
  const unsigned char mask[4] = {0x01, 0x02, 0x03, 0x04};

  const std::string small = W::encode_frame(0x1, "hi", mask);
  MPI_CHECK(small.size() == 2 + 4 + 2);
  MPI_CHECK(static_cast<unsigned char>(small[0]) == 0x81);   // FIN + text
  // A client frame must set the mask bit; a server rejects one that does not.
  MPI_CHECK(static_cast<unsigned char>(small[1]) == (0x80 | 2));
  MPI_CHECK(static_cast<unsigned char>(small[6]) == ('h' ^ 0x01));
  MPI_CHECK(static_cast<unsigned char>(small[7]) == ('i' ^ 0x02));

  // The 126 boundary: two bytes of length, big-endian.
  const std::string medium = W::encode_frame(0x1, std::string(200, 'x'), mask);
  MPI_CHECK(static_cast<unsigned char>(medium[1]) == (0x80 | 126));
  MPI_CHECK(static_cast<unsigned char>(medium[2]) == 0);
  MPI_CHECK(static_cast<unsigned char>(medium[3]) == 200);
  MPI_CHECK(medium.size() == 4 + 4 + 200);

  // Exactly 125 stays in the short form; exactly 126 does not.
  MPI_CHECK(static_cast<unsigned char>(
                W::encode_frame(0x1, std::string(125, 'x'), mask)[1]) ==
            (0x80 | 125));
  MPI_CHECK(static_cast<unsigned char>(
                W::encode_frame(0x1, std::string(126, 'x'), mask)[1]) ==
            (0x80 | 126));

  // And 65536 crosses into the eight-byte form.
  const std::string big = W::encode_frame(0x1, std::string(65536, 'x'), mask);
  MPI_CHECK(static_cast<unsigned char>(big[1]) == (0x80 | 127));
  MPI_CHECK(big.size() == 10 + 4 + 65536);
}

MPI_TEST(a_screenshot_says_when_it_was_taken, {}) {
  using namespace mpi::observe;
  // The rule this whole module exists for: an image is evidence of the
  // moment it was taken, and of nothing else. Each moment must say so, and
  // none of them may claim to show what a finding refers to.
  for (const ShotMoment m : {ShotMoment::kBeforeCapture,
                             ShotMoment::kDuringCapture,
                             ShotMoment::kAfterCapture,
                             ShotMoment::kUnrelated}) {
    const std::string note = evidence_note(m);
    MPI_CHECK_MSG(!note.empty(), "every moment explains itself");
    MPI_CHECK_MSG(std::string(to_string(m)) != "", "and has a wire name");
  }
  MPI_CHECK_MSG(std::string(evidence_note(ShotMoment::kAfterCapture))
                    .find("not evidence about any earlier moment") !=
                std::string::npos,
                "a picture taken afterwards denies being evidence of before");
  MPI_CHECK_MSG(std::string(evidence_note(ShotMoment::kDuringCapture))
                    .find("one instant") != std::string::npos,
                "and one taken during says it is a single instant");

  // A failed capture must not serialise as a real 0x0 image.
  Screenshot failed;
  failed.error = "adb could not be started";
  const std::string dumped = failed.to_json().dump();
  MPI_CHECK_MSG(dumped.find("\"width\":null") != std::string::npos,
                "an image that was not taken has no width, not width 0");
  MPI_CHECK_MSG(dumped.find("\"captured\":false") != std::string::npos,
                "and says it was not captured");
  MPI_CHECK_MSG(dumped.find("adb could not be started") != std::string::npos,
                "and carries the reason");
}

MPI_TEST(only_a_real_png_is_accepted_as_a_screenshot, {}) {
  using mpi::observe::png_dimensions;
  int w = -1, h = -1;
  // A minimal valid header: signature, length, IHDR, 4x4.
  std::string png;
  const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.append(reinterpret_cast<const char*>(sig), 8);
  png.append("\x00\x00\x00\x0D", 4);
  png.append("IHDR");
  png.append("\x00\x00\x00\x04", 4);   // width 4
  png.append("\x00\x00\x00\x07", 4);   // height 7
  MPI_CHECK(png_dimensions(png, &w, &h));
  MPI_CHECK(w == 4 && h == 7);

  // The cases a failed capture actually produces.
  MPI_CHECK_MSG(!png_dimensions("", &w, &h),
                "an empty file is not a 0x0 image");
  MPI_CHECK_MSG(!png_dimensions("not a png at all, just text", &w, &h),
                "and neither is an error message written to the file");
  MPI_CHECK_MSG(!png_dimensions(png.substr(0, 20), &w, &h),
                "a truncated header is refused rather than read short");
  // A pty-mangled stream -- \n turned into \r\n -- breaks the signature,
  // which is exactly why the Android path uses `exec-out` and not `shell`.
  std::string mangled = png;
  mangled.insert(3, "\x0D");
  MPI_CHECK_MSG(!png_dimensions(mangled, &w, &h),
                "a stream corrupted by a pty is refused");
  // A zero dimension is not legal PNG and would render as a broken image.
  std::string zero = png;
  zero[16] = 0; zero[17] = 0; zero[18] = 0; zero[19] = 0;
  MPI_CHECK(!png_dimensions(zero, &w, &h));
}

namespace {

mpi::rn::InspectorTarget rn_target(const char* app, const char* device,
                                   const char* description,
                                   const char* key = "devkey1") {
  mpi::rn::InspectorTarget t;
  t.app_id = app;
  t.device_name = device;
  t.description = description;
  t.device_key = key;
  t.websocket_path = std::string("/inspector/debug?device=") + key + "&page=1";
  return t;
}

}  // namespace

MPI_TEST(one_app_on_two_devices_is_a_question_not_a_guess, {}) {
  // The bug this pins, observed live: the same bundle id was attached from an
  // Android emulator and an iOS simulator at once, and asking for the app
  // always attached to Android. There was no way to reach iOS at all, and
  // nothing said a choice had been made.
  const std::vector<mpi::rn::InspectorTarget> targets = {
      rn_target("io.example.app", "sdk_gphone16k_arm64 - 17 - API 37",
                "React Native Bridgeless [C++ connection]", "androidkey"),
      // Two targets, one device: Metro lists the runtime connection and an
      // auxiliary page, and they share a device key.
      rn_target("io.example.app", "iPhone 17 Pro",
                "React Native Bridgeless [C++ connection]", "ioskey"),
      rn_target("io.example.app", "iPhone 17 Pro", "UI [C++ connection]",
                "ioskey"),
  };

  const auto blind = mpi::rn::choose_target(targets, "io.example.app", "");
  MPI_CHECK_MSG(blind.target == nullptr, "no target is chosen blindly");
  MPI_CHECK_MSG(blind.ambiguous, "it is reported as ambiguous");
  MPI_CHECK_MSG(blind.device_names.size() == 2,
                "two devices, not three targets: several targets on one "
                "device is normal and is resolved by preference");
  MPI_CHECK_MSG(!blind.error.empty(), "and the reason is stated");

  // Naming the device reaches it -- including the iOS one, which was
  // previously unreachable.
  const auto ios = mpi::rn::choose_target(targets, "io.example.app", "iPhone");
  MPI_CHECK(ios.target != nullptr);
  MPI_CHECK_MSG(ios.target->device_name == "iPhone 17 Pro",
                "the iOS device is selectable");
  MPI_CHECK_MSG(ios.target->description.find("Bridgeless") != std::string::npos,
                "and the full runtime connection wins over the UI page on "
                "the same device, since that is the one carrying Network");

  const auto droid = mpi::rn::choose_target(targets, "io.example.app", "sdk_gphone");
  MPI_CHECK(droid.target != nullptr);
  MPI_CHECK(droid.target->device_name.find("sdk_gphone") != std::string::npos);

  // Case does not decide it: nobody types a device name exactly.
  MPI_CHECK(mpi::rn::choose_target(targets, "io.example.app", "iphone").target
                != nullptr);
}

MPI_TEST(a_device_hint_that_matches_nothing_is_not_a_silent_fallback, {}) {
  const std::vector<mpi::rn::InspectorTarget> targets = {
      rn_target("io.example.app", "iPhone 17 Pro",
                "React Native Bridgeless [C++ connection]"),
  };
  const auto miss = mpi::rn::choose_target(targets, "io.example.app", "Pixel99");
  MPI_CHECK_MSG(miss.target == nullptr,
                "a hint that matches nothing attaches to nothing, rather "
                "than falling back to the only device and reporting it as "
                "the one asked for");
  MPI_CHECK(!miss.ambiguous);
  MPI_CHECK_MSG(!miss.device_names.empty(),
                "and the available devices are listed so the name can be "
                "corrected");

  // One device and no hint is unambiguous and must still work.
  const auto only = mpi::rn::choose_target(targets, "io.example.app", "");
  MPI_CHECK_MSG(only.target != nullptr,
                "a single device needs no hint");
}

MPI_TEST(another_apps_runtime_is_never_attached_to, {}) {
  const std::vector<mpi::rn::InspectorTarget> targets = {
      rn_target("io.other.app", "iPhone 17 Pro",
                "React Native Bridgeless [C++ connection]"),
  };
  const auto wrong = mpi::rn::choose_target(targets, "io.example.app", "");
  MPI_CHECK_MSG(wrong.target == nullptr,
                "a different app's runtime is not attached to: its traffic "
                "would be reported under the requested app's name");
  MPI_CHECK(wrong.error.find("io.example.app") != std::string::npos);
  // Even naming the device must not override the app mismatch.
  MPI_CHECK(mpi::rn::choose_target(targets, "io.example.app", "iPhone").target
                == nullptr);
}

MPI_TEST(two_devices_with_the_same_name_are_still_two_devices, {}) {
  // Device names are not unique. Two simulators of the same model on
  // different runtimes both report `iPad (A16)` -- verified against the
  // simulator list: UDIDs 7ABCF841... (iOS 18.6) and 1909934C... (iOS 26.5).
  //
  // Grouping candidates by name merged them into one and picked between them
  // silently, which is the very bug device selection was added to prevent.
  // Metro's `device=` parameter is the identity that distinguishes them.
  const std::vector<mpi::rn::InspectorTarget> targets = {
      rn_target("io.example.app", "iPad (A16)",
                "React Native Bridgeless [C++ connection]", "key18aaaaaa"),
      rn_target("io.example.app", "iPad (A16)",
                "React Native Bridgeless [C++ connection]", "key26bbbbbb"),
  };

  const auto blind = mpi::rn::choose_target(targets, "io.example.app", "");
  MPI_CHECK_MSG(blind.target == nullptr,
                "two same-named devices are not silently collapsed into one");
  MPI_CHECK_MSG(blind.ambiguous, "they are reported as a choice");
  MPI_CHECK_MSG(blind.device_names.size() == 2, "both are offered");
  // Offering the same string twice would be useless, so the listing has to
  // disambiguate them.
  MPI_CHECK_MSG(blind.device_names[0] != blind.device_names[1],
                "and the two entries are distinguishable, rather than the "
                "same name printed twice");
  bool shows_key = false;
  for (const auto& n : blind.device_names) {
    if (n.find("key18") != std::string::npos ||
        n.find("key26") != std::string::npos) {
      shows_key = true;
    }
  }
  MPI_CHECK_MSG(shows_key,
                "the device key is shown for exactly the names that clash");

  // And the key is how one of them is then selected: the name cannot do it.
  const auto by_key = mpi::rn::choose_target(targets, "io.example.app",
                                             "key26");
  MPI_CHECK(by_key.target != nullptr);
  MPI_CHECK_MSG(by_key.target->device_key == "key26bbbbbb",
                "the device key selects one of two identically named devices");

  // The shared name still narrows to those two and remains ambiguous, rather
  // than matching one arbitrarily.
  const auto by_name = mpi::rn::choose_target(targets, "io.example.app",
                                              "iPad");
  MPI_CHECK_MSG(by_name.target == nullptr && by_name.ambiguous,
                "a name matching both stays a question");
}

MPI_TEST(one_device_listed_twice_by_metro_is_not_ambiguous, {}) {
  // The common case, and it must not be mistaken for two devices: Metro
  // lists a runtime connection and its auxiliary pages for the same device.
  const std::vector<mpi::rn::InspectorTarget> targets = {
      rn_target("io.example.app", "iPhone 17 Pro",
                "React Native Bridgeless [C++ connection]", "same"),
      rn_target("io.example.app", "iPhone 17 Pro", "UI [C++ connection]",
                "same"),
  };
  const auto only = mpi::rn::choose_target(targets, "io.example.app", "");
  MPI_CHECK_MSG(only.target != nullptr && !only.ambiguous,
                "two targets on one device need no choice");
  MPI_CHECK_MSG(only.target->description.find("Bridgeless") != std::string::npos,
                "and the runtime connection is the one attached to");
}

MPI_TEST(request_detail_is_captured_only_when_asked_for, {}) {
  // Headers and bodies are what make a request inspectable rather than
  // listed, and they are where the secrets are: an Authorization header
  // carries a bearer token. Off unless asked.
  const char* kRequest =
      R"({"method":"Network.requestWillBeSent","params":{"requestId":"r1",
         "timestamp":100.0,"request":{"method":"POST",
         "url":"https://api.example.com/v2/login",
         "headers":{"Authorization":"Bearer secret-token",
                    "Content-Type":"application/json"},
         "postData":"{\"user\":\"someone\"}"}}})";
  mpi::json::ParseError err;

  {
    mpi::observe::InspectAssembler off;
    auto doc = mpi::json::parse(kRequest, &err);
    MPI_CHECK(doc.has_value());
    off.feed(*doc);
    const auto report = off.finish(1000);
    MPI_CHECK(report.network.size() == 1);
    MPI_CHECK_MSG(report.network.front().request_headers.empty(),
                  "no headers are kept by default");
    MPI_CHECK_MSG(!report.network.front().request_body.has_value(),
                  "and no request body");
    MPI_CHECK_MSG(report.to_json().dump().find("secret-token") ==
                      std::string::npos,
                  "so a bearer token cannot reach a report that nobody asked "
                  "to contain one");
  }

  mpi::observe::InspectAssembler on;
  on.capture_detail(true);
  auto doc = mpi::json::parse(kRequest, &err);
  MPI_CHECK(doc.has_value());
  on.feed(*doc);
  auto resp = mpi::json::parse(
      R"({"method":"Network.responseReceived","params":{"requestId":"r1",
         "response":{"status":200,"mimeType":"application/json",
         "headers":{"Content-Type":"application/json","Content-Length":"42"}}}})",
      &err);
  MPI_CHECK(resp.has_value());
  on.feed(*resp);
  auto fin = mpi::json::parse(
      R"({"method":"Network.loadingFinished","params":{"requestId":"r1",
         "timestamp":100.5,"encodedDataLength":42}})", &err);
  MPI_CHECK(fin.has_value());
  on.feed(*fin);

  // The assembler records what finished; asking for the body is the
  // session's job, because the assembler talks to nothing.
  MPI_CHECK_MSG(on.finished_requests().size() == 1,
                "a finished request is offered for a body fetch");
  MPI_CHECK(on.finished_requests().front() == "r1");
  on.set_response_body("r1", "{\"token\":\"abc\"}", /*base64=*/false);

  const auto report = on.finish(1000);
  const auto& ex = report.network.front();
  MPI_CHECK_MSG(ex.request_headers.size() == 2, "request headers are kept");
  MPI_CHECK_MSG(ex.request_headers.at("Authorization") == "Bearer secret-token",
                "verbatim -- redacting would be a claim about what was sent "
                "that is not true");
  MPI_CHECK(ex.response_headers.size() == 2);
  MPI_CHECK(ex.request_body.value_or("") == "{\"user\":\"someone\"}");
  MPI_CHECK(ex.response_body.value_or("") == "{\"token\":\"abc\"}");
  MPI_CHECK(!ex.response_body_base64);

  const std::string dumped = report.to_json().dump();
  MPI_CHECK(dumped.find("request_headers") != std::string::npos);
  MPI_CHECK(dumped.find("response_body") != std::string::npos);
  MPI_CHECK_MSG(dumped.find("\"response_body_base64\":false") != std::string::npos,
                "the encoding flag travels with the body: a reader decoding "
                "text as base64 gets nonsense");
}

MPI_TEST(a_base64_body_keeps_its_flag, {}) {
  // The real shape from the live inspector: `packager-status:running` came
  // back as base64 with base64Encoded true, for a response the runtime
  // classified as application/octet-stream.
  mpi::observe::InspectAssembler a;
  a.capture_detail(true);
  mpi::json::ParseError err;
  auto req = mpi::json::parse(
      R"({"method":"Network.requestWillBeSent","params":{"requestId":"r9",
         "timestamp":1.0,"request":{"method":"GET","url":"http://x/status"}}})",
      &err);
  MPI_CHECK(req.has_value());
  a.feed(*req);
  a.set_response_body("r9", "cGFja2FnZXItc3RhdHVzOnJ1bm5pbmc=", /*base64=*/true);
  const auto report = a.finish(10);
  MPI_CHECK(report.network.front().response_body_base64);
  MPI_CHECK(report.to_json().dump().find("\"response_body_base64\":true") !=
            std::string::npos);
}

MPI_TEST(no_body_by_construction_is_not_a_retrieval_failure, {}) {
  // The runtime reports "could not retrieve" for both a body it failed to
  // fetch and a response that never had one. A HEAD or a 204 is the second,
  // and printing the raw error for it reads as a failure.
  mpi::observe::InspectAssembler a;
  a.capture_detail(true);
  mpi::json::ParseError err;
  auto head = mpi::json::parse(
      R"({"method":"Network.requestWillBeSent","params":{"requestId":"h1",
         "timestamp":1.0,"request":{"method":"HEAD","url":"https://x/204"}}})",
      &err);
  MPI_CHECK(head.has_value());
  a.feed(*head);
  auto resp = mpi::json::parse(
      R"({"method":"Network.responseReceived","params":{"requestId":"h1",
         "response":{"status":204,"headers":{"Content-Length":"0"}}}})", &err);
  MPI_CHECK(resp.has_value());
  a.feed(*resp);
  a.set_response_body_unavailable(
      "h1", "Internal error: Could not retrieve response body for the given "
            "requestId.");
  const auto report = a.finish(10);
  const std::string why = report.network.front().response_body_unavailable;
  MPI_CHECK_MSG(why.find("no body by construction") != std::string::npos,
                "a HEAD is explained, not blamed on a retrieval error");
  MPI_CHECK_MSG(why.find("Internal error") == std::string::npos,
                "and the runtime's misleading wording is not passed through");

  // A response that should have had a body keeps the runtime's own words,
  // because then the retrieval really did fail.
  mpi::observe::InspectAssembler b;
  b.capture_detail(true);
  auto get = mpi::json::parse(
      R"({"method":"Network.requestWillBeSent","params":{"requestId":"g1",
         "timestamp":1.0,"request":{"method":"GET","url":"https://x/data"}}})",
      &err);
  MPI_CHECK(get.has_value());
  b.feed(*get);
  auto ok = mpi::json::parse(
      R"({"method":"Network.responseReceived","params":{"requestId":"g1",
         "response":{"status":200,"headers":{"Content-Length":"512"}}}})", &err);
  MPI_CHECK(ok.has_value());
  b.feed(*ok);
  b.set_response_body_unavailable("g1", "Internal error: gone");
  MPI_CHECK_MSG(b.finish(10).network.front().response_body_unavailable
                    .find("Internal error") != std::string::npos,
                "a real retrieval failure is reported as one");
}

// --- the state diff ---------------------------------------------------------
//
// This is the half of Redux detail that is observable read-only, so it is
// also the half that has to be right: it is what a reader will believe about
// what an action did.

namespace {

mpi::json::Value jv(const std::string& text) {
  mpi::json::ParseError err;
  auto v = mpi::json::parse(text, &err);
  MPI_CHECK_MSG(v.has_value(),
                "the test's own fixture must parse: " + err.message);
  return *v;
}

using Deltas = std::vector<mpi::observe::StateDelta>;

const mpi::observe::StateDelta* at(const Deltas& ds, const std::string& path) {
  for (const auto& d : ds) {
    if (d.path == path) return &d;
  }
  return nullptr;
}

Deltas diff(const std::string& before, const std::string& after,
            bool values = true, std::size_t max_deltas = 200,
            std::size_t max_depth = 12, std::string* trunc = nullptr) {
  std::string sink;
  return mpi::observe::diff_state(jv(before), jv(after), values, max_deltas,
                                  max_depth, trunc != nullptr ? trunc : &sink);
}

}  // namespace

MPI_TEST(an_unchanged_state_has_no_deltas, {}) {
  // The common case by a wide margin: most dispatches touch one slice, and a
  // diff that reported the other twelve would bury the one that matters.
  const std::string s = R"({"cart":{"items":[1,2]},"user":{"id":"u1"}})";
  MPI_CHECK(diff(s, s).empty());
}

MPI_TEST(a_changed_leaf_is_reported_at_its_full_path, {}) {
  const auto ds = diff(R"({"cart":{"items":[{"qty":1}]}})",
                       R"({"cart":{"items":[{"qty":3}]}})");
  MPI_CHECK_MSG(ds.size() == 1, "one change is one delta");
  const auto* d = at(ds, "cart.items[0].qty");
  MPI_CHECK_MSG(d != nullptr, "and the path names the leaf, not the slice");
  MPI_CHECK(d->kind == mpi::observe::StateDelta::Kind::kChanged);
  MPI_CHECK(d->before.has_value() && *d->before == "1");
  MPI_CHECK(d->after.has_value() && *d->after == "3");
}

MPI_TEST(added_and_removed_keys_carry_only_the_side_that_exists, {}) {
  // `null` is a value a Redux store can hold, so an added key must not render
  // its absent side as one -- that would be indistinguishable from a key
  // whose value really did change from null.
  const auto ds = diff(R"({"a":1,"gone":"x"})", R"({"a":1,"fresh":null})");
  const auto* added = at(ds, "fresh");
  const auto* removed = at(ds, "gone");
  MPI_CHECK(added != nullptr && removed != nullptr);
  MPI_CHECK(added->kind == mpi::observe::StateDelta::Kind::kAdded);
  MPI_CHECK_MSG(!added->before.has_value(),
                "an added key has no previous value at all");
  MPI_CHECK_MSG(added->after.has_value() && *added->after == "null",
                "and its new value is the null it actually holds");
  MPI_CHECK(removed->kind == mpi::observe::StateDelta::Kind::kRemoved);
  MPI_CHECK_MSG(!removed->after.has_value(),
                "a removed key has no new value at all");
}

MPI_TEST(values_are_omitted_entirely_when_not_requested, {}) {
  // A store holds bearer tokens and personal data. With values off, the path
  // is the whole finding -- and no value may leak into the record.
  const auto ds = diff(R"({"auth":{"token":"old-secret"}})",
                       R"({"auth":{"token":"new-secret"}})", /*values=*/false);
  MPI_CHECK(ds.size() == 1 && ds.front().path == "auth.token");
  MPI_CHECK_MSG(!ds.front().before.has_value() && !ds.front().after.has_value(),
                "neither side of a redacted diff carries a value");
}

MPI_TEST(a_longer_array_reports_the_appended_elements, {}) {
  const auto ds = diff(R"({"items":[1]})", R"({"items":[1,2,3]})");
  MPI_CHECK(ds.size() == 2);
  MPI_CHECK(at(ds, "items[1]") != nullptr && at(ds, "items[2]") != nullptr);
  MPI_CHECK(at(ds, "items[1]")->kind
            == mpi::observe::StateDelta::Kind::kAdded);
}

MPI_TEST(an_insertion_at_the_front_reports_every_later_index, {}) {
  // Index comparison, stated in the header as the honest cheap answer. A
  // person would call this one insertion; index 0 really does hold something
  // different, so reporting it is true even though it is verbose. This test
  // exists so the behaviour is a decision rather than a surprise.
  const auto ds = diff(R"({"q":[1,2,3]})", R"({"q":[0,1,2,3]})");
  MPI_CHECK(ds.size() == 4);
  MPI_CHECK(at(ds, "q[3]")->kind == mpi::observe::StateDelta::Kind::kAdded);
  MPI_CHECK(at(ds, "q[0]")->kind == mpi::observe::StateDelta::Kind::kChanged);
}

MPI_TEST(a_type_change_is_one_delta_not_a_walk_of_both_shapes, {}) {
  const auto ds = diff(R"({"v":{"a":1,"b":2}})", R"({"v":[1,2]})");
  MPI_CHECK(ds.size() == 1 && ds.front().path == "v");
  MPI_CHECK_MSG(ds.front().before.has_value() && *ds.front().before == "{2 keys}",
                "a container is summarised by shape, not dumped inline");
  MPI_CHECK(ds.front().after.has_value() && *ds.front().after == "[2 items]");
}

MPI_TEST(an_int_and_the_same_value_as_a_double_is_not_a_change, {}) {
  // A reducer that divides and lands on a whole number hands back 3 where it
  // held 3.0. Reporting that as a change would fill the list with noise on
  // every dispatch.
  MPI_CHECK(diff(R"({"n":3})", R"({"n":3.0})").empty());
  MPI_CHECK_MSG(diff(R"({"n":3})", R"({"n":3.5})").size() == 1,
                "while a real numeric change is still a change");
}

MPI_TEST(the_delta_cap_is_reported_rather_than_silently_shortening, {}) {
  std::string trunc;
  const auto ds = diff(R"({"a":1,"b":2,"c":3,"d":4})",
                       R"({"a":9,"b":9,"c":9,"d":9})", true, 2, 12, &trunc);
  MPI_CHECK(ds.size() == 2);
  MPI_CHECK_MSG(trunc.find("there were more") != std::string::npos,
                "a capped list says it is capped");
  MPI_CHECK(trunc.find("2 differences") != std::string::npos);
}

MPI_TEST(the_depth_cap_reports_the_path_it_stopped_at, {}) {
  std::string trunc;
  const auto ds = diff(R"({"a":{"b":{"c":{"d":1}}}})",
                       R"({"a":{"b":{"c":{"d":2}}}})", true, 200, 2, &trunc);
  MPI_CHECK_MSG(ds.size() == 1, "the change is still reported");
  MPI_CHECK_MSG(ds.front().path == "a.b",
                "at the depth the walk stopped, not at the leaf");
  MPI_CHECK_MSG(trunc.find("not at the leaf") != std::string::npos,
                "and the report says the path is not the leaf that changed");
}

MPI_TEST(a_clean_diff_reports_no_truncation, {}) {
  std::string trunc = "left over from a previous call";
  diff(R"({"a":1})", R"({"a":2})", true, 200, 12, &trunc);
  MPI_CHECK_MSG(trunc.empty(), "a complete diff must not claim to be partial");
}

MPI_TEST(a_rendered_value_is_single_line_and_cut_visibly, {}) {
  // Deltas sit in rows. A newline inside one breaks the row it is in, and a
  // value cut without a marker reads as the whole value.
  const auto multi = mpi::observe::render_delta_value(
      mpi::json::Value::string("line one\nline two"), 120);
  MPI_CHECK(multi.find('\n') == std::string::npos);
  MPI_CHECK(multi == "\"line one line two\"");
  const auto cut = mpi::observe::render_delta_value(
      mpi::json::Value::string(std::string(400, 'x')), 20);
  MPI_CHECK(cut.size() == 20);
  MPI_CHECK_MSG(cut.substr(cut.size() - 3) == "...",
                "a cut value says it was cut");
}

MPI_TEST(attribution_separates_an_observed_action_from_an_observed_change, {}) {
  // "the cart slice changed" and "ADD_TO_CART changed the cart slice" are
  // different claims. The record carries which one it is, and the note is
  // what stops a reader promoting the first into the second.
  const std::string sub = mpi::observe::attribution_note(
      mpi::observe::ReduxAttribution::kStateSubscription);
  MPI_CHECK_MSG(sub.find("no action is named") != std::string::npos,
                "a subscription record says why it names no action");
  const std::string wrap = mpi::observe::attribution_note(
      mpi::observe::ReduxAttribution::kDispatchWrapper);
  MPI_CHECK(wrap.find("the app dispatched") != std::string::npos);
  MPI_CHECK(sub != wrap);
}

MPI_TEST(a_slice_replaced_with_an_equal_value_is_a_finding_not_an_empty_row, {}) {
  // The classic Redux waste: a reducer returns `{...state}` on an action it
  // does not handle, so the reference changes, every subscriber watching the
  // slice re-renders, and nothing is different. It is invisible in a list of
  // action types and shows up here as a change with no differences.
  std::string err;
  mpi::json::ParseError perr;
  auto drain = mpi::json::parse(
      R"({"watching":true,"dropped":0,"records":[
           {"seq":1,"at":1000,"how":"dispatch","action_type":"PING",
            "changed":["cart"],
            "before":"{\"cart\":{\"items\":[1,2]}}",
            "after":"{\"cart\":{\"items\":[1,2]}}"}]})", &perr);
  MPI_CHECK(drain.has_value());
  mpi::observe::ReduxObservation out;
  mpi::observe::ingest_redux_drain(*drain, /*include_values=*/true, &out);
  MPI_CHECK(out.records.size() == 1);
  const auto& r = out.records.front();
  MPI_CHECK_MSG(r.changed_slices.size() == 1,
                "the slice's reference did change, and the app said so");
  MPI_CHECK_MSG(r.deltas.empty(), "and nothing inside it differs");
  MPI_CHECK_MSG(r.equal_replacement,
                "which is reported as a wasted replacement");
  MPI_CHECK(r.action_type.has_value() && *r.action_type == "PING");
}

MPI_TEST(a_real_change_is_not_called_an_equal_replacement, {}) {
  mpi::json::ParseError perr;
  auto drain = mpi::json::parse(
      R"({"watching":true,"records":[
           {"seq":1,"at":1000,"changed":["cart"],
            "before":"{\"cart\":{\"n\":1}}",
            "after":"{\"cart\":{\"n\":2}}"}]})", &perr);
  MPI_CHECK(drain.has_value());
  mpi::observe::ReduxObservation out;
  mpi::observe::ingest_redux_drain(*drain, true, &out);
  MPI_CHECK(out.records.front().deltas.size() == 1);
  MPI_CHECK(!out.records.front().equal_replacement);
}

MPI_TEST(without_values_an_empty_delta_list_claims_nothing, {}) {
  // Nobody compared anything, so calling this a wasted replacement would be
  // inventing a finding out of a setting that was switched off.
  mpi::json::ParseError perr;
  auto drain = mpi::json::parse(
      R"({"watching":true,"records":[
           {"seq":1,"at":1000,"changed":["cart"],
            "before":"{\"cart\":1}","after":"{\"cart\":2}"}]})", &perr);
  MPI_CHECK(drain.has_value());
  mpi::observe::ReduxObservation out;
  mpi::observe::ingest_redux_drain(*drain, /*include_values=*/false, &out);
  MPI_CHECK(out.records.front().deltas.empty());
  MPI_CHECK_MSG(!out.records.front().equal_replacement,
                "an uncompared change is not an equal one");
}

MPI_TEST(a_bypassed_dispatch_is_not_read_as_an_unwatched_one, {}) {
  // Wrapped, and the change still arrived with no action: a thunk's injected
  // dispatch does not pass through `store.dispatch`. Reporting it as a plain
  // subscription record would imply nothing was wrapped.
  mpi::json::ParseError perr;
  auto drain = mpi::json::parse(
      R"({"watching":true,"records":[
           {"seq":1,"at":1000,"how":"bypassed","changed":["profile"]},
           {"seq":2,"at":1001,"how":"subscribe","changed":["cart"]}]})", &perr);
  MPI_CHECK(drain.has_value());
  mpi::observe::ReduxObservation out;
  mpi::observe::ingest_redux_drain(*drain, false, &out);
  MPI_CHECK(out.records.size() == 2);
  MPI_CHECK_MSG(out.records[0].dispatch_bypassed,
                "a bypassed dispatch is marked as one");
  MPI_CHECK_MSG(!out.records[1].dispatch_bypassed,
                "and a read-only record is not");
  MPI_CHECK_MSG(!out.records[0].action_type.has_value(),
                "neither carries an invented action type");
}

MPI_TEST(a_watcher_that_is_gone_does_not_read_as_an_idle_store, {}) {
  // The app reloaded. The records already held came from a runtime that no
  // longer exists, and the list stops growing -- silence would read as an
  // app that stopped dispatching.
  mpi::json::ParseError perr;
  auto drain = mpi::json::parse(
      R"({"watching":false,"note":"no watcher is installed -- the app reloaded"})",
      &perr);
  MPI_CHECK(drain.has_value());
  mpi::observe::ReduxObservation out;
  out.records.push_back({});
  mpi::observe::ingest_redux_drain(*drain, true, &out);
  MPI_CHECK_MSG(out.records.size() == 1,
                "what was already collected is kept, not discarded");
  MPI_CHECK_MSG(out.note.find("reloaded") != std::string::npos,
                "and the reason the list stopped growing is stated");
}

MPI_TEST(the_in_app_buffers_overflow_is_carried_not_hidden, {}) {
  mpi::json::ParseError perr;
  auto first = mpi::json::parse(
      R"({"watching":true,"dropped":7,"records":[{"seq":1,"at":1,"changed":[]}]})",
      &perr);
  auto second = mpi::json::parse(
      R"({"watching":true,"dropped":5,"records":[{"seq":2,"at":2,"changed":[]}]})",
      &perr);
  MPI_CHECK(first.has_value() && second.has_value());
  mpi::observe::ReduxObservation out;
  mpi::observe::ingest_redux_drain(*first, false, &out);
  mpi::observe::ingest_redux_drain(*second, false, &out);
  MPI_CHECK_MSG(out.dropped == 12,
                "drops accumulate across drains rather than being replaced");
  MPI_CHECK(out.records.size() == 2);
}
