// Android collector parsers, tested against REAL output captured from a live
// Android emulator (API 37, sdk_gphone16k_arm64) on 2026-09-14.
//
// Every fixture here is `.real.` -- genuine `dumpsys` and `simpleperf` output,
// including the not-debuggable refusal, which is the case the capability
// contract exists to report honestly.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "adapters/android/adb_collector.hpp"
#include "core/session/live_capture.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::android;

namespace {

std::string read_fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  const std::string path = std::string(dir ? dir : "fixtures") + "/" + rel;
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

MPI_TEST(framestats_parses_real_emulator_output, {"E01", "J14"}) {
  const auto text = read_fixture("provider-output/android-gfxinfo-framestats.real.txt");
  MPI_CHECK_MSG(!text.empty(), "the real framestats fixture is missing");
  const auto fs = parse_gfxinfo_framestats(text);
  MPI_CHECK_MSG(fs.rows.size() > 20,
                "expected the captured frames, got " +
                    std::to_string(fs.rows.size()));
  for (const auto& r : fs.rows) {
    MPI_CHECK_MSG(r.intended_vsync > 0, "every row needs a frame start");
    MPI_CHECK_MSG(r.frame_completed > 0, "every row needs a completion time");
    MPI_CHECK_MSG(r.frame_deadline > r.intended_vsync,
                  "the platform's deadline must be after the frame start");
  }
}

MPI_TEST(framestats_yields_the_refresh_rate_from_the_platform, {"E01", "E02"}) {
  const auto fs = parse_gfxinfo_framestats(
      read_fixture("provider-output/android-gfxinfo-framestats.real.txt"));
  MPI_CHECK_MSG(fs.observed_frame_intervals.size() == 1,
                "this capture ran at one steady rate, got " +
                    std::to_string(fs.observed_frame_intervals.size()) +
                    " distinct intervals");
  // 16666666 ns is 60 Hz. The point is that it is *read*, not assumed.
  MPI_CHECK_EQ(fs.observed_frame_intervals.front(),
               static_cast<std::int64_t>(16666666));
}

MPI_TEST(framestats_reads_the_header_rather_than_fixed_columns, {"D18"}) {
  // A reordered header with the same fields must still parse correctly, since
  // the column set has changed across Android releases.
  const std::string reordered =
      "---PROFILEDATA---\n"
      "Flags,FrameCompleted,IntendedVsync,FrameDeadline,FrameInterval,\n"
      "0,200,100,150,16666666,\n"
      "---PROFILEDATA---\n";
  const auto fs = parse_gfxinfo_framestats(reordered);
  MPI_CHECK_EQ(fs.rows.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(fs.rows[0].intended_vsync, static_cast<std::int64_t>(100));
  MPI_CHECK_EQ(fs.rows[0].frame_deadline, static_cast<std::int64_t>(150));
  MPI_CHECK_EQ(fs.rows[0].frame_completed, static_cast<std::int64_t>(200));
}

MPI_TEST(framestats_refuses_a_header_missing_required_columns, {"D18", "A12"}) {
  const std::string missing =
      "---PROFILEDATA---\n"
      "Flags,Vsync,SwapBuffers,\n"
      "0,100,200,\n"
      "---PROFILEDATA---\n";
  const auto fs = parse_gfxinfo_framestats(missing);
  MPI_CHECK_MSG(fs.rows.empty(),
                "a header without a deadline must not be parsed by guesswork");
  MPI_CHECK_MSG(!fs.warnings.empty(), "the refusal must be reported");
}

MPI_TEST(framestats_skips_platform_excluded_frames, {"E03", "E04"}) {
  // A non-zero Flags value marks a frame the platform says to exclude from
  // jank statistics (first draw, window layout change).
  const std::string with_flagged =
      "---PROFILEDATA---\n"
      "Flags,IntendedVsync,FrameDeadline,FrameCompleted,FrameInterval,\n"
      "1,100,150,900,16666666,\n"
      "0,200,250,260,16666666,\n"
      "---PROFILEDATA---\n";
  const auto fs = parse_gfxinfo_framestats(with_flagged);
  MPI_CHECK_MSG(fs.rows.size() == 1,
                "the flagged frame must be excluded, not counted as a huge miss");
  MPI_CHECK_EQ(fs.rows[0].intended_vsync, static_cast<std::int64_t>(200));
}

MPI_TEST(framestats_handles_empty_and_headerless_blocks, {"D16", "A12"}) {
  // This is what the platform returns when nothing rendered since the last
  // read: a block containing only its header. It must yield no frames and no
  // warning, because nothing is wrong.
  const std::string empty_block =
      "---PROFILEDATA---\n"
      "Flags,IntendedVsync,FrameDeadline,FrameCompleted,\n"
      "---PROFILEDATA---\n";
  const auto fs = parse_gfxinfo_framestats(empty_block);
  MPI_CHECK(fs.rows.empty());
  MPI_CHECK(fs.warnings.empty());
  MPI_CHECK(parse_gfxinfo_framestats("").rows.empty());
  MPI_CHECK(parse_gfxinfo_framestats("no profiledata here\n").rows.empty());
}

MPI_TEST(simpleperf_parses_real_samples_and_callchains, {"DET-04", "J14"}) {
  const auto text =
      read_fixture("provider-output/android-simpleperf-report-sample.real.txt");
  MPI_CHECK_MSG(!text.empty(), "the real simpleperf fixture is missing");
  const auto parsed = parse_simpleperf_report_sample(text);
  MPI_CHECK_MSG(!parsed.samples.empty(), "expected real samples");
  for (const auto& s : parsed.samples) {
    MPI_CHECK_MSG(s.time_ns > 0, "every sample needs a timestamp");
    MPI_CHECK_MSG(!s.frames.empty(), "a sample with no frame must be dropped");
    MPI_CHECK_MSG(!s.thread_name.empty(), "every sample carries its thread name");
  }
}

MPI_TEST(simpleperf_stacks_are_outermost_first, {"E15"}) {
  // simpleperf emits the leaf first; the model wants outermost first, because
  // that is the order DET-04 presents a candidate stack in.
  const std::string sample =
      "sample:\n"
      "  time: 1000\n"
      "  event_count: 10000\n"
      "  thread_id: 42\n"
      "  thread_name: main\n"
      "  symbol: leafFunction\n"
      "  callchain:\n"
      "    symbol: middleFunction\n"
      "    symbol: outermostFunction\n";
  const auto parsed = parse_simpleperf_report_sample(sample);
  MPI_CHECK_EQ(parsed.samples.size(), static_cast<std::size_t>(1));
  const auto& f = parsed.samples[0].frames;
  MPI_CHECK_EQ(f.size(), static_cast<std::size_t>(3));
  MPI_CHECK_EQ(f.front(), std::string("outermostFunction"));
  MPI_CHECK_EQ(f.back(), std::string("leafFunction"));
}

MPI_TEST(simpleperf_meta_info_yields_real_build_facts, {"C11", "C18"}) {
  const auto parsed = parse_simpleperf_report_sample(
      read_fixture("provider-output/android-simpleperf-report-sample.real.txt"));
  // These are build facts read off the device, which is stronger than anything
  // inferred host-side.
  MPI_CHECK_EQ(parsed.app_package_name, std::string("io.pizzahut.hutbot.debug"));
  MPI_CHECK_MSG(parsed.app_type == "debuggable",
                "the captured app really is a debug build, got '" +
                    parsed.app_type + "'");
  MPI_CHECK_EQ(parsed.build_type, std::string("user"));
  MPI_CHECK(parsed.sdk_version.has_value());
  MPI_CHECK_MSG(*parsed.sdk_version >= 30,
                "expected a modern API level, got " +
                    std::to_string(*parsed.sdk_version));
  MPI_CHECK_EQ(parsed.event_type, std::string("cpu-clock"));
}

MPI_TEST(simpleperf_finds_the_react_native_js_thread, {"G01", "DET-02"}) {
  // The real capture is a React Native debug build, so its JS thread must be
  // present. Its name is "mqt_v_js" on this RN version, which is why the
  // collector matches more than just "mqt_js".
  const auto parsed = parse_simpleperf_report_sample(
      read_fixture("provider-output/android-simpleperf-report-sample.real.txt"));
  bool found_js = false;
  for (const auto& s : parsed.samples) {
    if (s.thread_name.rfind("mqt_", 0) == 0) found_js = true;
  }
  MPI_CHECK_MSG(found_js,
                "expected a React Native JS thread in the real capture");
}

MPI_TEST(simpleperf_handles_the_not_debuggable_refusal, {"A02", "J14"}) {
  // REAL output: simpleperf refusing a non-debuggable package. The parser must
  // produce no samples and not mistake the error for data.
  const auto text =
      read_fixture("provider-output/android-simpleperf-not-debuggable.real.txt");
  MPI_CHECK_MSG(!text.empty(), "the refusal fixture is missing");
  MPI_CHECK(text.find("debuggable/profileable") != std::string::npos);
  const auto parsed = parse_simpleperf_report_sample(text);
  MPI_CHECK(parsed.samples.empty());
  MPI_CHECK_MSG(!parsed.warnings.empty(),
                "output that yielded no usable sample must say so");
}

MPI_TEST(simpleperf_handles_empty_input, {"D16"}) {
  const auto parsed = parse_simpleperf_report_sample("");
  MPI_CHECK(parsed.samples.empty());
  MPI_CHECK_MSG(parsed.warnings.empty(),
                "empty input is not an anomaly worth warning about");
}

MPI_TEST(meminfo_parses_real_output_and_keeps_families_distinct, {"F08", "J14"}) {
  const auto text = read_fixture("provider-output/android-meminfo.real.txt");
  MPI_CHECK_MSG(!text.empty(), "the real meminfo fixture is missing");
  const auto mem = parse_dumpsys_meminfo(text);
  MPI_CHECK(mem.pid.has_value());
  MPI_CHECK(mem.rss_total_bytes.has_value());
  MPI_CHECK(mem.pss_total_bytes.has_value());
  MPI_CHECK(mem.native_heap_rss_bytes.has_value());
  MPI_CHECK(mem.dalvik_heap_rss_bytes.has_value());
  // Column order is pinned to the exact values in the fixture rather than to
  // an inequality between families. A first attempt asserted RSS >= PSS, which
  // looks obviously true and is not: this capture has 324918 KB swapped out,
  // and Android's TOTAL row reports PSS 567594 KB against RSS 365440 KB. That
  // is the "platform metrics are not automatically equivalent" trap from spec
  // section 8, and it is why these families are never compared or summed.
  const double kKiB = 1024.0;
  MPI_CHECK_NEAR(*mem.pss_total_bytes, 567594 * kKiB, 1.0);
  MPI_CHECK_NEAR(*mem.rss_total_bytes, 365440 * kKiB, 1.0);
  MPI_CHECK_NEAR(*mem.private_dirty_bytes, 36112 * kKiB, 1.0);
  MPI_CHECK_MSG(*mem.pss_total_bytes > *mem.rss_total_bytes,
                "this particular capture really does report PSS above RSS "
                "because of swap; the parser must reproduce the platform's "
                "numbers rather than a sanitised version of them");
  // The families are separate measurements and must not collapse together.
  MPI_CHECK_MSG(*mem.native_heap_rss_bytes != *mem.dalvik_heap_rss_bytes,
                "the native and managed heaps are distinct measurements");
}

MPI_TEST(meminfo_absent_fields_stay_absent, {"C18", "section-8"}) {
  const auto mem = parse_dumpsys_meminfo("nothing useful\n");
  MPI_CHECK_MSG(!mem.rss_total_bytes.has_value(),
                "an unseen counter must be absent, not zero");
  MPI_CHECK(!mem.pid.has_value());
}

MPI_TEST(collector_refuses_an_empty_process_set, {"B15"}) {
  AdbCollector collector;
  model::DeviceRef device;
  device.device_id = "emulator-5554";
  device.platform = model::Platform::kAndroid;
  device.trust = model::TrustState::kAuthorized;
  session::CaptureConfig cfg;
  model::NormalizedTrace trace;
  const auto r = collector.capture(device, {}, cfg, trace);
  MPI_CHECK(!r.started);
  MPI_CHECK(!r.ok());
  MPI_CHECK(r.error.find("no process instance") != std::string::npos);
}

MPI_TEST(collector_refuses_an_option_like_identifier, {"A20", "J05"}) {
  AdbCollector collector;
  model::DeviceRef device;
  device.device_id = "emulator-5554";
  device.trust = model::TrustState::kAuthorized;
  model::ProcessInstance p;
  p.app.app_identifier = "--all";
  p.pid = 1;
  p.is_primary = true;
  session::CaptureConfig cfg;
  model::NormalizedTrace trace;
  const auto r = collector.capture(device, {p}, cfg, trace);
  MPI_CHECK(!r.started);
  MPI_CHECK(r.error.find("command-line option") != std::string::npos);
}

MPI_TEST(capture_config_records_its_preset_and_sources, {"I17"}) {
  // Two runs are only comparable when they used the same collector
  // configuration, so the configuration has to be recorded.
  session::CaptureConfig cfg;
  cfg.preset = "heavy";
  cfg.sample_frequency_hz = 1000;
  cfg.memory = false;
  const auto j = cfg.to_json();
  MPI_CHECK_EQ(j.find("preset")->as_string(), std::string("heavy"));
  MPI_CHECK_EQ(j.find("sample_frequency_hz")->as_int(),
               static_cast<std::int64_t>(1000));
  MPI_CHECK_EQ(j.find("memory")->as_bool(), false);
  MPI_CHECK_EQ(j.find("frames")->as_bool(), true);
  MPI_CHECK(j.find("reset_frame_history") != nullptr);
}

// --- streaming ---------------------------------------------------------------

MPI_TEST(streaming_is_declared_and_batch_is_not_removed, {"section-13"}) {
  AdbCollector collector;
  MPI_CHECK(collector.supports_streaming());
  MPI_CHECK_EQ(collector.id(), std::string("android.adb.text-sources"));
}

MPI_TEST(streaming_refuses_to_begin_without_a_process, {"B15"}) {
  AdbCollector collector;
  model::DeviceRef device;
  device.device_id = "emulator-5554";
  device.trust = model::TrustState::kAuthorized;
  session::CaptureConfig cfg;
  model::NormalizedTrace trace;
  const auto r = collector.begin(device, {}, cfg, trace);
  MPI_CHECK(!r.started);
  MPI_CHECK(r.error.find("no process instance") != std::string::npos);
}

MPI_TEST(streaming_refuses_an_option_like_identifier, {"A20", "J05"}) {
  AdbCollector collector;
  model::DeviceRef device;
  device.device_id = "emulator-5554";
  device.trust = model::TrustState::kAuthorized;
  model::ProcessInstance p;
  p.app.app_identifier = "--all";
  p.pid = 1;
  p.is_primary = true;
  session::CaptureConfig cfg;
  model::NormalizedTrace trace;
  const auto r = collector.begin(device, {p}, cfg, trace);
  MPI_CHECK(!r.started);
  MPI_CHECK(r.error.find("command-line option") != std::string::npos);
}

MPI_TEST(live_update_serialises_its_deltas_and_cost, {"section-13"}) {
  session::LiveUpdate u;
  u.at_ns = 1234;
  u.new_frames = 7;
  u.new_cpu_samples = 3;
  u.new_counter_points = 5;
  u.tick_cost = std::chrono::milliseconds(180);
  u.notes.push_back("a note");
  const auto j = u.to_json();
  MPI_CHECK_EQ(j.find("new_frames")->as_int(), static_cast<std::int64_t>(7));
  MPI_CHECK_EQ(j.find("new_cpu_samples")->as_int(), static_cast<std::int64_t>(3));
  MPI_CHECK_EQ(j.find("new_counter_points")->as_int(), static_cast<std::int64_t>(5));
  // The collector's own cost is reported, never folded into the app's numbers.
  MPI_CHECK_EQ(j.find("tick_cost_ms")->as_int(), static_cast<std::int64_t>(180));
  MPI_CHECK_EQ(j.find("notes")->size(), static_cast<std::size_t>(1));
}

MPI_TEST(capture_config_records_the_streaming_cadence, {"I17"}) {
  session::CaptureConfig cfg;
  cfg.tick_interval = std::chrono::milliseconds(250);
  cfg.cpu_window = std::chrono::milliseconds(8000);
  const auto j = cfg.to_json();
  // Two runs are only comparable when they streamed at the same cadence, so
  // the cadence is part of the recorded configuration.
  MPI_CHECK_EQ(j.find("tick_interval_ms")->as_int(), static_cast<std::int64_t>(250));
  MPI_CHECK_EQ(j.find("cpu_window_ms")->as_int(), static_cast<std::int64_t>(8000));
}

MPI_TEST(live_snapshot_states_that_it_is_preliminary, {"section-2.2", "section-13"}) {
  session::LiveSnapshot snap;
  snap.state = session::LiveState::kRunning;
  snap.analysis_is_preliminary = true;
  const auto j = snap.to_json();
  MPI_CHECK_EQ(j.find("analysis_is_preliminary")->as_bool(), true);
  // The caveat lives in the document, not in a UI label that could be lost in
  // translation.
  MPI_CHECK(j.find("analysis_caveat")->as_string().find("PRELIMINARY") !=
            std::string::npos);
  MPI_CHECK(j.find("analysis_caveat")->as_string().find("still open") !=
            std::string::npos);

  snap.analysis_is_preliminary = false;
  const auto done = snap.to_json();
  MPI_CHECK_EQ(done.find("analysis_is_preliminary")->as_bool(), false);
  MPI_CHECK(done.find("analysis_caveat")->as_string().find("final") !=
            std::string::npos);
}

MPI_TEST(live_session_refuses_a_non_streaming_collector, {"section-13"}) {
  // A collector that does not stream must be refused rather than silently
  // producing an empty live session.
  class BatchOnly final : public session::Collector {
   public:
    std::string id() const override { return "test.batch-only"; }
    model::Platform platform() const override { return model::Platform::kAndroid; }
    session::CaptureResult capture(const model::DeviceRef&,
                                   const std::vector<model::ProcessInstance>&,
                                   const session::CaptureConfig&,
                                   model::NormalizedTrace&) override {
      return session::CaptureResult{};
    }
  };
  session::LiveSession live;
  model::DeviceRef device;
  device.device_id = "d";
  model::ProcessInstance p;
  p.pid = 1;
  const bool started = live.start(std::make_shared<BatchOnly>(), device, {p},
                                 session::CaptureConfig{},
                                 model::NormalizedTrace{});
  MPI_CHECK(!started);
  const auto snap = live.snapshot();
  MPI_CHECK(snap.state == session::LiveState::kFailed);
  MPI_CHECK(snap.error.find("does not support streaming") != std::string::npos);
}

MPI_TEST(live_session_start_stop_is_safe_without_a_collector, {"J11"}) {
  session::LiveSession live;
  model::DeviceRef device;
  MPI_CHECK(!live.start(nullptr, device, {}, session::CaptureConfig{},
                        model::NormalizedTrace{}));
  MPI_CHECK(!live.running());
  // stop() on a session that never ran must not hang or crash.
  live.stop();
  live.stop();
  MPI_CHECK(!live.running());
}

MPI_TEST(uptime_is_read_or_refused_never_defaulted, {"E13", "section-8"}) {
  // `dumpsys meminfo` reports no timestamp, so a memory point is placed by
  // the host against one reading of the device's boot time. If that reading
  // cannot be trusted the capture must say so rather than anchor to zero:
  // the first version of this stamped the opening samples of every capture at
  // 0, which put them before the capture window began.
  double v = -1.0;
  MPI_CHECK(android::parse_leading_double("7574.59 30298.12", v));
  MPI_CHECK_EQ(v, 7574.59);

  v = -1.0;
  MPI_CHECK(android::parse_leading_double("  12.34\n", v));
  MPI_CHECK_EQ(v, 12.34);

  // Anything unreadable is refused outright, and leaves the output alone.
  for (const char* junk : {"", "   ", "error: no such file", "abc 12.0"}) {
    double untouched = -1.0;
    MPI_CHECK(!android::parse_leading_double(junk, untouched));
    MPI_CHECK_EQ(untouched, -1.0);
  }

  // A well-formed zero parses, and it is the caller that rejects it as a boot
  // time -- the parser does not conflate "unreadable" with "zero".
  double zero = -1.0;
  MPI_CHECK(android::parse_leading_double("0.00 0.00", zero));
  MPI_CHECK_EQ(zero, 0.0);
}
