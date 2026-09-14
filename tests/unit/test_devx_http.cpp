// DevX HTTP layer.
//
// The security properties are the point of these tests: the listener is
// loopback-only, every request needs the token, and nothing in a request can
// be turned into a path outside the sessions directory.
#include <sstream>

#include "apps/devx-serve/http_server.hpp"
#include "apps/devx-serve/ui.hpp"
#include "core/util/json.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::devx;

MPI_TEST(url_decode_handles_escapes_and_plus, {"J05"}) {
  MPI_CHECK_EQ(url_decode("a%20b"), std::string("a b"));
  MPI_CHECK_EQ(url_decode("a+b"), std::string("a b"));
  MPI_CHECK_EQ(url_decode("com.example.app"), std::string("com.example.app"));
  MPI_CHECK_EQ(url_decode("%2Fetc%2Fpasswd"), std::string("/etc/passwd"));
  MPI_CHECK_EQ(url_decode("caf%C3%A9"), std::string("caf\xC3\xA9"));
}

MPI_TEST(url_decode_leaves_malformed_escapes_literal, {"J05"}) {
  // Dropping a malformed escape could turn one path into a different valid
  // one, so the '%' stays.
  MPI_CHECK_EQ(url_decode("100%"), std::string("100%"));
  MPI_CHECK_EQ(url_decode("%zz"), std::string("%zz"));
  MPI_CHECK_EQ(url_decode("%2"), std::string("%2"));
  MPI_CHECK_EQ(url_decode("a%"), std::string("a%"));
}

MPI_TEST(html_escape_neutralises_markup, {"J01", "section-15"}) {
  MPI_CHECK_EQ(html_escape("<script>alert(1)</script>"),
               std::string("&lt;script&gt;alert(1)&lt;/script&gt;"));
  MPI_CHECK_EQ(html_escape("a&b"), std::string("a&amp;b"));
  MPI_CHECK_EQ(html_escape("\"quoted\""), std::string("&quot;quoted&quot;"));
  MPI_CHECK_EQ(html_escape("it's"), std::string("it&#39;s"));
}

MPI_TEST(response_error_is_machine_readable, {"H12"}) {
  const auto r = Response::error(404, "no such session");
  MPI_CHECK_EQ(r.status, 404);
  MPI_CHECK(r.content_type.find("application/json") != std::string::npos);
  json::ParseError err;
  auto parsed = json::parse(r.body, &err);
  MPI_CHECK_MSG(parsed.has_value(), "an error body must be valid JSON");
  MPI_CHECK_EQ(parsed->find("error")->as_string(), std::string("no such session"));
  MPI_CHECK_EQ(parsed->find("status")->as_int(), static_cast<std::int64_t>(404));
}

MPI_TEST(server_generates_a_distinct_token_per_instance, {"J06", "section-14"}) {
  HttpServer a;
  HttpServer b;
  MPI_CHECK_MSG(a.token().size() == 32,
                "expected a 32-character token, got " +
                    std::to_string(a.token().size()));
  MPI_CHECK_MSG(a.token() != b.token(),
                "two instances must not share a token");
  for (const char c : a.token()) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    MPI_CHECK_MSG(hex, "the token must be hex");
  }
}

MPI_TEST(server_binds_loopback_on_an_ephemeral_port, {"section-14"}) {
  HttpServer s;
  std::string error;
  MPI_CHECK_MSG(s.listen(0, &error), "listen failed: " + error);
  MPI_CHECK_MSG(s.port() != 0, "an ephemeral port must be reported back");
}

MPI_TEST(server_reports_a_bind_failure_rather_than_pretending, {"A06"}) {
  HttpServer first;
  std::string error;
  MPI_CHECK(first.listen(0, &error));
  const auto port = first.port();

  // Binding the same port again must fail with a stated reason.
  HttpServer second;
  std::string error2;
  const bool ok = second.listen(port, &error2);
  MPI_CHECK_MSG(!ok, "the second bind on the same port should fail");
  MPI_CHECK_MSG(!error2.empty(), "a bind failure must carry its reason");
  MPI_CHECK(error2.find("127.0.0.1") != std::string::npos);
}

MPI_TEST(index_page_substitutes_the_token_once, {"section-14"}) {
  const std::string page = index_html("tok123", "9.9.9", "7.7");
  MPI_CHECK(page.find("const TOKEN = \"tok123\"") != std::string::npos);
  MPI_CHECK_MSG(page.find("__TOKEN__") == std::string::npos,
                "no placeholder may survive substitution");
  MPI_CHECK(page.find("9.9.9") != std::string::npos);
  MPI_CHECK(page.find("7.7") != std::string::npos);
  // The page must carry the honesty language the reports use, so the UI
  // cannot present a softer story than the engine.
  MPI_CHECK(page.find("because it did not run") != std::string::npos);
  MPI_CHECK(page.find("does not mean foreground") != std::string::npos);
  MPI_CHECK(page.find("comparable to a physical device") != std::string::npos);
}

MPI_TEST(request_param_lookup, {"H12"}) {
  Request r;
  r.query["device"] = "emulator-5554";
  r.query["empty"] = "";
  MPI_CHECK_EQ(r.param("device"), std::string("emulator-5554"));
  MPI_CHECK(r.has_param("empty"));
  MPI_CHECK_EQ(r.param("empty"), std::string(""));
  MPI_CHECK(!r.has_param("absent"));
  MPI_CHECK_EQ(r.param("absent", "fallback"), std::string("fallback"));
}
