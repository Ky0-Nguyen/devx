// Checking for a newer DevX: the version comparison, reading GitHub's
// "latest release" reply, and what the one request carries and where it goes.
#include <string>
#include <vector>

#include "core/update/update.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

using mpi::net::HttpRequest;
using mpi::net::HttpResponse;
namespace update = mpi::update;

const char* kRelease = R"({
  "tag_name": "0.6.1",
  "name": "DevX 0.6.1",
  "body": "## What changed\n- something",
  "published_at": "2026-11-10T08:00:00Z",
  "html_url": "https://github.com/Ky0-Nguyen/devx/releases/tag/0.6.1",
  "assets": [
    {"name": "DevX-0.6.1.dmg", "browser_download_url": "https://github.com/Ky0-Nguyen/devx/releases/download/0.6.1/DevX-0.6.1.dmg"},
    {"name": "DevX-0.6.1.dmg.sha256", "browser_download_url": "https://github.com/Ky0-Nguyen/devx/releases/download/0.6.1/DevX-0.6.1.dmg.sha256"},
    {"name": "notes.txt", "browser_download_url": "https://example.test/notes.txt"}
  ]
})";

HttpResponse reply(int status, std::string body) {
  HttpResponse r;
  r.ok = true;
  r.status = status;
  r.body = std::move(body);
  return r;
}

}  // namespace

MPI_TEST(update_compares_versions_numerically, {}) {
  MPI_CHECK_EQ(update::compare_versions("0.5.10", "0.5.9"), 1);
  MPI_CHECK_EQ(update::compare_versions("0.5.9", "0.5.10"), -1);
  MPI_CHECK_EQ(update::compare_versions("1.2", "1.2.0"), 0);
  MPI_CHECK_EQ(update::compare_versions("v0.6.0", "0.6.0"), 0);
  MPI_CHECK_EQ(update::compare_versions("1.0.0", "0.99.99"), 1);
  // A pre-release is older than the release with the same numbers.
  MPI_CHECK_EQ(update::compare_versions("0.7.0-beta", "0.7.0"), -1);
  MPI_CHECK_EQ(update::compare_versions("0.7.0-beta", "0.6.9"), 1);
}

MPI_TEST(update_reads_the_latest_release, {}) {
  const auto o = update::parse_latest_release(kRelease, "0.6.0");
  MPI_CHECK(o.find("ok")->as_bool());
  MPI_CHECK_EQ(o.find("latest")->as_string(), std::string("0.6.1"));
  MPI_CHECK(o.find("newer")->as_bool());
  MPI_CHECK_EQ(o.find("dmg_name")->as_string(), std::string("DevX-0.6.1.dmg"));
  MPI_CHECK(o.find("dmg_url")->as_string().find("/DevX-0.6.1.dmg") != std::string::npos);
  MPI_CHECK(o.find("sha256_url")->as_string().find(".dmg.sha256") != std::string::npos);
  MPI_CHECK(o.find("notes")->as_string().find("What changed") != std::string::npos);
}

MPI_TEST(update_says_up_to_date_and_never_offers_a_downgrade, {}) {
  MPI_CHECK(!update::parse_latest_release(kRelease, "0.6.1").find("newer")->as_bool());
  MPI_CHECK(!update::parse_latest_release(kRelease, "0.7.0").find("newer")->as_bool());
}

MPI_TEST(update_ignores_assets_that_are_not_the_release_image, {}) {
  const std::string body = R"({"tag_name":"v0.6.1","assets":[
    {"name":"DevX-0.6.1.dmg","browser_download_url":"http://insecure.test/DevX-0.6.1.dmg"},
    {"name":"Other.dmg","browser_download_url":"https://github.com/x/Other.dmg"}]})";
  const auto o = update::parse_latest_release(body, "0.6.0");
  MPI_CHECK(o.find("ok")->as_bool());
  MPI_CHECK_EQ(o.find("latest")->as_string(), std::string("0.6.1"));
  // Not https, and not the expected name: neither is offered for install.
  MPI_CHECK(o.find("dmg_url")->as_string().empty());
  MPI_CHECK(o.find("sha256_url")->as_string().empty());
}

MPI_TEST(update_refuses_a_reply_that_is_not_a_release, {}) {
  MPI_CHECK(!update::parse_latest_release("not json", "0.6.0").find("ok")->as_bool());
  MPI_CHECK(!update::parse_latest_release(R"({"message":"x"})", "0.6.0").find("ok")->as_bool());
}

MPI_TEST(update_asks_github_only_and_names_itself, {}) {
  std::vector<HttpRequest> seen;
  const auto o = update::check("0.6.0", [&](const HttpRequest& r) {
    seen.push_back(r);
    return reply(200, kRelease);
  });
  MPI_CHECK(o.find("ok")->as_bool());
  MPI_CHECK_EQ(seen.size(), std::size_t(1));
  MPI_CHECK_EQ(seen[0].url, std::string("https://api.github.com/repos/Ky0-Nguyen/devx/releases/latest"));
  MPI_CHECK(seen[0].allowed_hosts == std::vector<std::string>{"api.github.com"});
  MPI_CHECK(seen[0].body.empty());
  bool agent = false;
  for (const auto& [k, v] : seen[0].headers) {
    if (k == "User-Agent") agent = v == "DevX/0.6.0";
    // Nothing that identifies the person or carries a credential.
    MPI_CHECK(k != "Authorization" && k != "Cookie");
  }
  MPI_CHECK(agent);
}

MPI_TEST(update_reports_rate_limits_and_missing_releases_in_words, {}) {
  auto limited = update::check("0.6.0", [](const HttpRequest&) {
    HttpResponse r = reply(403, "{}");
    r.headers["x-ratelimit-remaining"] = "0";
    return r;
  });
  MPI_CHECK(!limited.find("ok")->as_bool());
  MPI_CHECK(limited.find("error")->as_string().find("limit") != std::string::npos);
  auto none = update::check("0.6.0", [](const HttpRequest&) { return reply(404, "{}"); });
  MPI_CHECK(none.find("error")->as_string().find("no release") != std::string::npos);
  auto offline = update::check("0.6.0", [](const HttpRequest&) {
    HttpResponse r;
    r.error = "could not resolve host";
    return r;
  });
  MPI_CHECK(offline.find("error")->as_string().find("could not reach GitHub") != std::string::npos);
}

