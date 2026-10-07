// BrowserStack: the App Profiling series reader, and the pieces of the
// App Automate client that do not need the network.
//
// BrowserStack documents the profiling summary but not the shape of each
// `*_data` series. The first test is that shape as BrowserStack served it for
// a Pixel 9 (Android 16) session; the others pin the fallbacks.
#include "adapters/browserstack/automate.hpp"
#include "adapters/browserstack/profiling.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

using mpi::browserstack::parse_series;
using mpi::browserstack::Series;

mpi::json::Value doc(const char* text) {
  auto v = mpi::json::parse(text, nullptr);
  return v ? *v : mpi::json::Value();
}

const Series* field(const std::vector<Series>& all, const std::string& name) {
  for (const auto& s : all) {
    if (s.field == name) return &s;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(series_as_browserstack_serves_them, {}) {
  // Two quantities in one reply, a null that is no sample, and times that are
  // offsets from the session's start -- kept, so CPU and memory line up.
  const auto all = parse_series(doc(R"([
      {"time_offset_ms": 3053, "cpu_usage": 1.72, "cpu_threads": 39, "batt_usage": null},
      {"time_offset_ms": 4069, "cpu_usage": 1.02, "cpu_threads": 38, "batt_usage": null}])"),
                                "");
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(2));
  const Series* cpu = field(all, "cpu_usage");
  MPI_CHECK(cpu != nullptr);
  MPI_CHECK_EQ(cpu->points.size(), static_cast<std::size_t>(2));
  MPI_CHECK_EQ(cpu->points[0].first, static_cast<std::int64_t>(3053000000));
  MPI_CHECK(cpu->points[1].second == 1.02);
  MPI_CHECK(field(all, "cpu_threads") != nullptr);
  MPI_CHECK_MSG(field(all, "batt_usage") == nullptr, "a null is not a sample of zero");
}

MPI_TEST(series_from_pairs_in_epoch_milliseconds, {}) {
  const auto all = parse_series(doc("[[1700000000500, 12.5], [1700000000000, 10]]"), "");
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(1));
  const auto& s = all[0].points;
  MPI_CHECK_EQ(s[0].first, static_cast<std::int64_t>(0));  // sorted, from the first sample
  MPI_CHECK_EQ(s[1].first, static_cast<std::int64_t>(500000000));
  MPI_CHECK(s[1].second == 12.5);
}

MPI_TEST(series_from_objects_inside_a_data_wrapper, {}) {
  const auto all = parse_series(
      doc(R"({"data": [{"timestamp": 1700000000, "value": 3}, {"timestamp": 1700000002, "value": 4}]})"),
      "");
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(all[0].points[1].first, static_cast<std::int64_t>(2000000000));  // seconds
  MPI_CHECK(all[0].points[1].second == 4);
}

MPI_TEST(series_from_csv_with_a_header, {}) {
  const auto all = parse_series(mpi::json::Value(), "time,reads,writes\n10,1.5,0\n11,2.5,7\n");
  MPI_CHECK_EQ(all.size(), static_cast<std::size_t>(2));
  const Series* w = field(all, "writes");
  MPI_CHECK(w != nullptr);
  MPI_CHECK_EQ(w->points[1].first, static_cast<std::int64_t>(1000000000));
  MPI_CHECK(w->points[1].second == 7);
}

MPI_TEST(series_of_an_unknown_shape_is_empty_not_guessed, {}) {
  MPI_CHECK(parse_series(doc(R"({"summary": {"avg": 3}})"), "").empty());
  MPI_CHECK(parse_series(mpi::json::Value(), "a,b\n1,2\n").empty());  // no time column
  MPI_CHECK(parse_series(doc(R"([{"cpu_usage": 3}])"), "").empty());  // no time at all
}

MPI_TEST(base64_and_png_header_for_screenshots, {}) {
  std::string out;
  MPI_CHECK(mpi::browserstack::base64_decode("aGVsbG8=", &out));
  MPI_CHECK_EQ(out, std::string("hello"));
  MPI_CHECK(!mpi::browserstack::base64_decode("a*b", &out));

  // The 8-byte signature, then IHDR's length, type, width 1080, height 2424.
  const std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x04\x38\0\0\x09\x78", 24);
  int w = 0, h = 0;
  MPI_CHECK(mpi::browserstack::png_size(png, &w, &h));
  MPI_CHECK_EQ(w, 1080);
  MPI_CHECK_EQ(h, 2424);
  MPI_CHECK(!mpi::browserstack::png_size("GIF89a not a png at all.", &w, &h));
}

MPI_TEST(automate_capabilities_send_only_the_settings_asked_for, {}) {
  mpi::browserstack::AutomateSpec spec;
  spec.app_url = "bs://abc";
  spec.platform = "android";
  spec.device = "Google Pixel 9";
  spec.os_version = "16.0";
  spec.network_profile = "4g-lte-good";
  spec.app_profiling = true;
  const auto caps = spec.capabilities();
  const auto* always = caps.find("capabilities")->find("alwaysMatch");
  MPI_CHECK_EQ(always->find("appium:app")->as_string(), std::string("bs://abc"));
  const auto* bs = always->find("bstack:options");
  MPI_CHECK_EQ(bs->find("deviceName")->as_string(), std::string("Google Pixel 9"));
  MPI_CHECK_EQ(bs->find("networkProfile")->as_string(), std::string("4g-lte-good"));
  MPI_CHECK(bs->find("appProfiling")->as_bool());
  MPI_CHECK_MSG(bs->find("gpsLocation") == nullptr && bs->find("enableBiometric") == nullptr,
                "a setting not asked for is not sent, so BrowserStack's default holds");
  MPI_CHECK(always->find("appium:locale") == nullptr);
}
