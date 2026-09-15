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
