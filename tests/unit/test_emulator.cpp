// The emulator module's protocol stack and files: protobuf, HPACK, the HTTP/2
// and gRPC client, the SDK manifest reader, and AVD files.
//
// HPACK is checked against RFC 7541's own worked examples (Appendix C), which
// is the point: a decoder that only agrees with its own encoder proves
// nothing. The gRPC client runs against a minimal HTTP/2 server inside this
// test that sends exactly the frames a real server does. Against the real
// emulator the stack was exercised by hand (getStatus, getScreenshot,
// streamScreenshot over MMAP, touch, keys, rotation, Extended Controls) and
// against Node's HTTP/2 server for HPACK; neither is available in CI.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "adapters/android/emulator/avd.hpp"
#include "adapters/android/emulator/sdk_repository.hpp"
#include "core/net/grpc_client.hpp"
#include "core/net/hpack.hpp"
#include "core/util/protobuf.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

std::string unhex(const std::string& h) {
  std::string out;
  std::string digits;
  for (char c : h) {
    if (std::isxdigit(static_cast<unsigned char>(c))) digits.push_back(c);
  }
  for (std::size_t i = 0; i + 1 < digits.size(); i += 2) {
    out.push_back(static_cast<char>(std::strtol(digits.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

bool has(const mpi::net::Headers& h, const std::string& k, const std::string& v) {
  for (const auto& [a, b] : h) {
    if (a == k && b == v) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(protobuf_round_trips_every_wire_type, {}) {
  mpi::pb::Writer inner;
  inner.string(1, "hello");
  mpi::pb::Writer w;
  w.varint(1, 300);
  w.int32(2, -5);
  w.dbl(3, 2.5);
  w.flt(4, 1.25f);
  w.message(5, inner);
  w.boolean(6, true);
  mpi::pb::Reader r(w.data());
  mpi::pb::Field f;
  int seen = 0;
  while (r.next(f)) {
    seen++;
    if (f.number == 1) MPI_CHECK_EQ(f.value, static_cast<std::uint64_t>(300));
    if (f.number == 2) MPI_CHECK_EQ(f.as_int32(), -5);
    if (f.number == 3) MPI_CHECK_EQ(f.as_double(), 2.5);
    if (f.number == 4) MPI_CHECK_EQ(f.as_float(), 1.25f);
    if (f.number == 5) {
      mpi::pb::Reader ir(f.bytes);
      mpi::pb::Field g;
      MPI_CHECK(ir.next(g));
      MPI_CHECK_EQ(std::string(g.bytes), std::string("hello"));
    }
    if (f.number == 6) MPI_CHECK_EQ(f.value, static_cast<std::uint64_t>(1));
  }
  MPI_CHECK_EQ(seen, 6);
  MPI_CHECK(r.error().empty());
}

MPI_TEST(protobuf_refuses_a_length_past_the_end, {}) {
  const std::string bad = std::string("\x0a\x05hi", 4);  // field 1, length 5, two bytes
  mpi::pb::Reader r(bad);
  mpi::pb::Field f;
  MPI_CHECK(!r.next(f));
  MPI_CHECK(!r.error().empty());
}

MPI_TEST(hpack_decodes_rfc7541_c3_requests_with_the_dynamic_table, {}) {
  mpi::net::HpackDecoder d;
  mpi::net::Headers h1, h2;
  // C.3.1 and C.3.2: literal :authority is indexed, then referenced as 62.
  MPI_CHECK(d.decode(unhex("8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d"), h1));
  MPI_CHECK(has(h1, ":method", "GET") && has(h1, ":scheme", "http") && has(h1, ":path", "/"));
  MPI_CHECK(has(h1, ":authority", "www.example.com"));
  MPI_CHECK(d.decode(unhex("8286 84be 5808 6e6f 2d63 6163 6865"), h2));
  MPI_CHECK(has(h2, ":authority", "www.example.com"));
  MPI_CHECK(has(h2, "cache-control", "no-cache"));
}

MPI_TEST(hpack_decodes_rfc7541_c4_huffman_requests, {}) {
  mpi::net::HpackDecoder d;
  mpi::net::Headers h1, h2;
  MPI_CHECK(d.decode(unhex("8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff"), h1));
  MPI_CHECK(has(h1, ":authority", "www.example.com"));
  MPI_CHECK(d.decode(unhex("8286 84be 5886 a8eb 1064 9cbf"), h2));
  MPI_CHECK(has(h2, "cache-control", "no-cache"));
}

MPI_TEST(huffman_round_trips_every_byte_value, {}) {
  std::string all;
  for (int i = 0; i < 256; i++) all.push_back(static_cast<char>(i));
  std::string back;
  MPI_CHECK(mpi::net::huffman_decode(mpi::net::huffman_encode(all), back));
  MPI_CHECK(back == all);
  // RFC 7541 C.4.1: "www.example.com".
  MPI_CHECK_EQ(mpi::net::huffman_encode("www.example.com"), unhex("f1e3 c2e5 f23a 6ba0 ab90 f4ff"));
}

MPI_TEST(huffman_refuses_padding_that_is_not_eos, {}) {
  std::string out;
  // 'a' is 00011 (5 bits); padding with zeros instead of ones is an error.
  MPI_CHECK(!mpi::net::huffman_decode(std::string(1, static_cast<char>(0x18)), out));
}

MPI_TEST(hpack_encoding_is_read_back_by_the_decoder, {}) {
  const mpi::net::Headers in = {{":method", "POST"}, {"authorization", "Bearer x"}};
  mpi::net::HpackDecoder d;
  mpi::net::Headers out;
  MPI_CHECK(d.decode(mpi::net::hpack_encode(in), out));
  MPI_CHECK(out == in);
}

namespace {

// A minimal HTTP/2 gRPC server for one connection: answers each request with
// `reply` and the trailers `status`, or with nothing at all when `silent`.
struct FakeGrpc {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread t;

  void start(std::string reply, int status, bool silent) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    ::listen(listen_fd, 1);
    t = std::thread([this, reply, status, silent] {
      const int c = ::accept(listen_fd, nullptr, nullptr);
      auto frame = [&](std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                       const std::string& p) {
        std::string f;
        f.push_back(static_cast<char>((p.size() >> 16) & 0xff));
        f.push_back(static_cast<char>((p.size() >> 8) & 0xff));
        f.push_back(static_cast<char>(p.size() & 0xff));
        f.push_back(static_cast<char>(type));
        f.push_back(static_cast<char>(flags));
        for (int i = 3; i >= 0; i--) f.push_back(static_cast<char>((stream >> (8 * i)) & 0xff));
        f += p;
        const ssize_t n = ::send(c, f.data(), f.size(), 0);
        static_cast<void>(n);
      };
      char pre[24];
      ::recv(c, pre, sizeof pre, MSG_WAITALL);
      frame(4, 0, 0, "");  // SETTINGS
      for (;;) {
        unsigned char h[9];
        if (::recv(c, h, 9, MSG_WAITALL) != 9) break;
        const std::uint32_t n = (static_cast<std::uint32_t>(h[0]) << 16) |
                                (static_cast<std::uint32_t>(h[1]) << 8) | h[2];
        std::string p(n, '\0');
        if (n > 0 && ::recv(c, p.data(), n, MSG_WAITALL) != static_cast<ssize_t>(n)) break;
        const std::uint32_t stream = (static_cast<std::uint32_t>(h[5] & 0x7f) << 24) |
                                     (static_cast<std::uint32_t>(h[6]) << 16) |
                                     (static_cast<std::uint32_t>(h[7]) << 8) | h[8];
        if (h[3] == 0 && (h[4] & 1) && !silent) {  // the request's last DATA
          if (status == 0) {
            // :status 200 (static index 8), then the message, then trailers.
            frame(1, 4, stream, std::string("\x88", 1) + mpi::net::hpack_encode({{"content-type", "application/grpc"}}));
            frame(0, 0, stream, mpi::net::grpc_frame(reply));
            frame(1, 5, stream, mpi::net::hpack_encode({{"grpc-status", "0"}}));
          } else {
            // Trailers-only: one HEADERS ends the stream with the status.
            frame(1, 5, stream, std::string("\x88", 1) +
                                    mpi::net::hpack_encode({{"grpc-status", std::to_string(status)},
                                                            {"grpc-message", "no%20token"}}));
          }
        }
      }
      ::close(c);
    });
  }
  ~FakeGrpc() {
    ::shutdown(listen_fd, SHUT_RDWR);
    ::close(listen_fd);
    if (t.joinable()) t.join();
  }
};

}  // namespace

MPI_TEST(a_unary_call_returns_the_servers_message, {}) {
  FakeGrpc s;
  s.start("pong", 0, false);
  mpi::net::GrpcChannel ch(s.port, "Bearer t");
  std::string err;
  MPI_CHECK_MSG(ch.connect(&err), err);
  std::string resp;
  const auto st = ch.unary("/x.Y/Ping", "ping", &resp, std::chrono::seconds(3));
  MPI_CHECK_MSG(st.ok(), st.message);
  MPI_CHECK_EQ(resp, std::string("pong"));
  ch.close();
}

MPI_TEST(a_trailers_only_failure_carries_its_status_and_decoded_message, {}) {
  FakeGrpc s;
  s.start("", 16, false);
  mpi::net::GrpcChannel ch(s.port, "");
  std::string err;
  MPI_CHECK(ch.connect(&err));
  std::string resp;
  const auto st = ch.unary("/x.Y/Ping", "", &resp, std::chrono::seconds(3));
  MPI_CHECK_EQ(st.code, 16);
  MPI_CHECK_EQ(st.message, std::string("no token"));
  ch.close();
}

MPI_TEST(a_server_that_never_answers_times_out_rather_than_hanging, {}) {
  FakeGrpc s;
  s.start("", 0, true);
  mpi::net::GrpcChannel ch(s.port, "");
  std::string err;
  MPI_CHECK(ch.connect(&err));
  const auto began = std::chrono::steady_clock::now();
  const auto st = ch.unary("/x.Y/Ping", "", nullptr, std::chrono::milliseconds(400));
  MPI_CHECK(!st.ok());
  MPI_CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(3));
  ch.close();
}

MPI_TEST(nothing_listening_is_a_connect_error, {}) {
  mpi::net::GrpcChannel ch(1, "");
  std::string err;
  MPI_CHECK(!ch.connect(&err));
  MPI_CHECK(!err.empty());
}

MPI_TEST(the_manifest_keeps_stable_packages_for_this_host_only, {}) {
  const std::string xml = R"(<?xml version="1.0"?>
<sdk:sdk-repository xmlns:sdk="http://x" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<license id="lic" type="text">  The terms.  </license>
<channel id="channel-0">stable</channel>
<remotePackage path="emulator"><revision><major>37</major><minor>2</minor><micro>12</micro></revision>
<display-name>Android Emulator</display-name><uses-license ref="lic"/><channelRef ref="channel-0"/>
<archives><archive><complete><size>10</size><checksum type="sha1">aa</checksum><url>emu-linux.zip</url></complete><host-os>linux</host-os></archive>
<archive><complete><size>20</size><checksum type="sha1">bb</checksum><url>emu-mac-arm.zip</url></complete><host-os>macosx</host-os><host-arch>aarch64</host-arch></archive></archives></remotePackage>
<remotePackage path="emulator"><revision><major>38</major></revision><channelRef ref="channel-3"/>
<archives><archive><complete><size>1</size><checksum type="sha1">cc</checksum><url>canary.zip</url></complete></archive></archives></remotePackage>
<remotePackage path="system-images;android-35;google_apis;x86_64"><type-details xsi:type="x"><api-level>35</api-level><tag><id>google_apis</id><display>Google APIs</display></tag><abi>x86_64</abi></type-details>
<revision><major>1</major></revision><channelRef ref="channel-0"/><archives><archive><complete><size>5</size><checksum type="sha1">dd</checksum><url>x86.zip</url></complete></archive></archives></remotePackage>
</sdk:sdk-repository>)";
  mpi::android::Host host;
  host.arch = "aarch64";
  host.abi = "arm64-v8a";
  mpi::android::SdkCatalog cat;
  mpi::android::parse_manifest(xml, "https://dl.google.com/android/repository/", host, cat);
  MPI_CHECK_EQ(cat.packages.size(), static_cast<std::size_t>(1));
  const auto& p = cat.packages[0];
  MPI_CHECK_EQ(p.path, std::string("emulator"));
  MPI_CHECK_EQ(p.revision, std::string("37.2.12"));
  MPI_CHECK_EQ(p.archive.url, std::string("https://dl.google.com/android/repository/emu-mac-arm.zip"));
  MPI_CHECK_EQ(p.archive.sha1, std::string("bb"));
  MPI_CHECK_EQ(p.license_id, std::string("lic"));
  MPI_CHECK(cat.licenses.count("lic") == 1);
}

MPI_TEST(a_license_is_recorded_as_sdkmanager_records_it, {}) {
  // sha1("abc"): the text is trimmed before hashing.
  MPI_CHECK_EQ(mpi::android::license_hash("  abc \n"),
               std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
}

MPI_TEST(window_size_classes_follow_the_600_and_840_dp_boundaries, {}) {
  using mpi::android::window_size_class;
  MPI_CHECK_EQ(window_size_class(1080, 420), std::string("compact"));   // 411 dp
  MPI_CHECK_EQ(window_size_class(700, 160), std::string("medium"));     // 700 dp
  MPI_CHECK_EQ(window_size_class(599, 160), std::string("compact"));
  MPI_CHECK_EQ(window_size_class(600, 160), std::string("medium"));
  MPI_CHECK_EQ(window_size_class(840, 160), std::string("expanded"));
  for (const auto& p : mpi::android::device_presets()) {
    MPI_CHECK_MSG(p.width > 0 && p.height > 0 && p.density > 0, p.id);
  }
}

MPI_TEST(an_avd_is_created_resized_and_deleted_as_plain_files, {}) {
  namespace fs = std::filesystem;
  const fs::path home = fs::temp_directory_path() / ("devx-avd-test-" + std::to_string(::getpid()));
  const fs::path sdk = home / "sdk";
  fs::create_directories(sdk / "system-images/android-35/google_apis/arm64-v8a");
  std::ofstream(sdk / "system-images/android-35/google_apis/arm64-v8a/system.img") << "x";
  ::setenv("ANDROID_AVD_HOME", (home / "avd").c_str(), 1);
  fs::create_directories(home / "avd");

  mpi::android::AvdSpec spec;
  spec.display_name = "Test Phone";
  spec.system_image = "system-images;android-35;google_apis;arm64-v8a";
  spec.width = 1080;
  spec.height = 2400;
  spec.density = 420;
  mpi::android::AvdInfo a;
  std::string err;
  MPI_CHECK_MSG(mpi::android::create_avd(sdk.string(), spec, a, &err), err);
  MPI_CHECK_EQ(a.id, std::string("Test_Phone"));
  MPI_CHECK_EQ(a.api_level, 35);
  MPI_CHECK_EQ(a.width, 1080);
  MPI_CHECK(!mpi::android::create_avd(sdk.string(), spec, a, &err));  // exists already
  MPI_CHECK_EQ(mpi::android::list_avds().size(), static_cast<std::size_t>(1));

  MPI_CHECK(mpi::android::set_avd_screen("Test_Phone", 1766, 2208, 420, &err));
  MPI_CHECK(mpi::android::read_avd("Test_Phone", a, &err));
  MPI_CHECK_EQ(a.width, 1766);
  MPI_CHECK_MSG(fs::exists(fs::path(a.directory) / mpi::android::kColdBootMarker),
                "a changed screen asks for one cold boot");

  spec.system_image = "system-images;android-99;google_apis;arm64-v8a";
  spec.display_name = "Missing";
  MPI_CHECK(!mpi::android::create_avd(sdk.string(), spec, a, &err));
  MPI_CHECK(err.find("not installed") != std::string::npos);

  MPI_CHECK(mpi::android::delete_avd("Test_Phone", &err));
  MPI_CHECK(mpi::android::list_avds().empty());
  ::unsetenv("ANDROID_AVD_HOME");
  fs::remove_all(home);
}
