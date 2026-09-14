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
#include "adapters/android/atrace_parser.hpp"
#include "core/rules/engine.hpp"
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


MPI_TEST(am_start_w_parses_a_real_cold_launch, {"DET-07", "J14"}) {
  const auto r = android::parse_am_start_w(
      read_fixture("provider-output/android-am-start-w-cold.real.txt"));
  MPI_CHECK(r.started);
  MPI_CHECK_EQ(r.status, std::string("ok"));
  // The launch class is the platform's own classification, never inferred
  // from how long the launch took.
  MPI_CHECK_EQ(r.launch_state, std::string("COLD"));
  MPI_CHECK_EQ(r.component,
               std::string("io.pizzahut.hutbot.debug/io.yum.MainActivity"));
  MPI_CHECK(r.total_time_ns.has_value());
  MPI_CHECK_EQ(*r.total_time_ns, model::TimeNs{4889} * 1000000);
  MPI_CHECK(r.wait_time_ns.has_value());
  MPI_CHECK_EQ(*r.wait_time_ns, model::TimeNs{4907} * 1000000);
  MPI_CHECK(r.refusal.empty());
}

MPI_TEST(am_start_w_refuses_the_zero_of_an_app_already_running,
         {"DET-07", "E13", "section-8"}) {
  // The real trap in this provider. Re-launching an app that is already
  // foreground prints TotalTime: 0 with a warning, and a parser that takes
  // the number at face value reports a zero-millisecond startup that sits
  // comfortably inside any budget. The zero is a missing measurement.
  const auto r = android::parse_am_start_w(
      read_fixture("provider-output/android-am-start-w-already-running.real.txt"));
  MPI_CHECK(!r.started);
  MPI_CHECK_EQ(r.status, std::string("ok"));  // the command itself succeeded
  MPI_CHECK(!r.total_time_ns.has_value());
  MPI_CHECK(!r.wait_time_ns.has_value());
  MPI_CHECK(!r.refusal.empty());
  MPI_CHECK(r.refusal.find("not a startup duration") != std::string::npos);
  MPI_CHECK(!r.warnings.empty());
}

MPI_TEST(displayed_log_parses_the_platform_first_frame_figure, {"DET-07", "J14"}) {
  const auto recs = android::parse_displayed_log(
      read_fixture("provider-output/android-displayed-logcat.real.txt"));
  MPI_CHECK_EQ(recs.size(), std::size_t{1});
  if (recs.empty()) return;
  MPI_CHECK_EQ(recs.front().component,
               std::string("io.pizzahut.hutbot.debug/io.yum.MainActivity"));
  // "+4s889ms"
  MPI_CHECK_EQ(recs.front().elapsed_ns, model::TimeNs{4889} * 1000000);
}

MPI_TEST(displayed_log_reads_each_unit_rather_than_assuming_a_shape, {"D18"}) {
  const auto recs = android::parse_displayed_log(
      "I ActivityTaskManager: Displayed a/.A for user 0: +687ms\n"
      "I ActivityTaskManager: Displayed b/.B for user 0: +1m2s3ms\n"
      "I ActivityTaskManager: Displayed c/.C for user 0: +12s\n"
      "I ActivityTaskManager: nothing to see here\n");
  MPI_CHECK_EQ(recs.size(), std::size_t{3});
  if (recs.size() < 3) return;
  MPI_CHECK_EQ(recs[0].elapsed_ns, model::TimeNs{687} * 1000000);
  MPI_CHECK_EQ(recs[1].elapsed_ns,
               (model::TimeNs{62} * 1000000000) + (model::TimeNs{3} * 1000000));
  MPI_CHECK_EQ(recs[2].elapsed_ns, model::TimeNs{12} * 1000000000);
}

MPI_TEST(resolve_activity_reads_the_component_not_the_details, {"A21"}) {
  MPI_CHECK_EQ(android::parse_resolved_activity(
                   read_fixture("provider-output/android-resolve-activity.real.txt")),
               std::string("io.pizzahut.hutbot.debug/io.yum.MainActivity"));
  // A package with no launchable activity is a real answer, not an error.
  MPI_CHECK(android::parse_resolved_activity("No activity found\n").empty());
}


// --- atrace, mapped into the model, then read by DET-03 and DET-09 ----------
//
// Driven from the committed ftrace text rather than a recorded session: the
// same six-second capture as a normalized trace is three megabytes, and the
// text is the real provider output anyway.

namespace {

// The app's threads as /proc/<pid>/task reported them during the capture the
// fixture came from.
std::vector<android::AppThread> fixture_threads() {
  return {
      {3378, "ut.hutbot.debug"},   // tid == pid: the UI main thread
      {3440, "SharedPreferenc"},   // the thread that blocked on a page read
      {3452, "ScionFrontendAp"},
      {3454, "Firebase Backgr"},
      {3457, "WM.task-1"},
  };
}

model::NormalizedTrace mapped_fixture() {
  const auto parsed = android::parse_atrace(
      read_fixture("provider-output/android-atrace-cold-start.real.txt"));
  model::NormalizedTrace out;
  out.session_id = "atrace-fixture";
  out.primary_clock_domain = "android.boottime.ns";
  android::map_atrace_to_trace(parsed, fixture_threads(), 3378,
                               "android|emulator|io.pizzahut.hutbot.debug|pid=3378",
                               out);
  return out;
}

const model::RuleRunRecord* record_for_rule(const model::AnalysisResult& r,
                                            const std::string& id) {
  for (const auto& rec : r.rule_runs) {
    if (rec.rule_id == id) return &rec;
  }
  return nullptr;
}

bool has_text(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v) {
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(atrace_mapping_attributes_only_the_apps_threads, {"B01", "E17", "F11"}) {
  const auto trace = mapped_fixture();

  // The trace is system-wide. Every event kept must belong to a thread this
  // app owns; anything else is out of scope, not the app being idle.
  MPI_CHECK(!trace.events.empty());
  const auto threads = fixture_threads();
  for (const auto& e : trace.events) {
    bool owned = false;
    for (const auto& t : threads) {
      if (e.thread_instance_id == "tid=" + std::to_string(t.tid)) owned = true;
    }
    MPI_CHECK_MSG(owned, "event attributed to a thread the app does not own: " +
                             e.thread_instance_id);
  }

  // Thread identity, including which one is the main thread -- from the
  // platform's convention (tid == pid), not from the name.
  const model::ThreadInfo* main = nullptr;
  for (const auto& t : trace.threads) {
    if (t.is_main_ui_thread) main = &t;
  }
  MPI_CHECK(main != nullptr);
  if (main != nullptr) {
    MPI_CHECK_EQ(main->tid, 3378);
    MPI_CHECK_EQ(main->name, std::string("ut.hutbot.debug"));
  }
}

MPI_TEST(atrace_mapping_keeps_states_apart_and_only_calls_iowait_io,
         {"E10", "DET-03", "C18"}) {
  const auto trace = mapped_fixture();
  std::size_t io_events = 0;
  std::size_t uninterruptible = 0;
  std::size_t runnable = 0;
  for (const auto& e : trace.events) {
    if (e.category == model::EventCategory::kIo) {
      ++io_events;
      // An I/O event exists only where the kernel said iowait=1.
      const auto* flag = e.payload.find("iowait");
      MPI_CHECK(flag != nullptr && flag->is_bool() && flag->as_bool());
      // And it carries where the kernel blocked, which is the only location
      // this provider gives.
      const auto* caller = e.payload.find("kernel_caller");
      MPI_CHECK(caller != nullptr && !caller->as_string().empty());
    }
    if (e.category != model::EventCategory::kSchedule) continue;
    if (e.name == "uninterruptible") ++uninterruptible;
    if (e.name == "runnable") ++runnable;
  }
  MPI_CHECK_MSG(io_events > 0, "the fixture contains real iowait blocks");
  MPI_CHECK_MSG(uninterruptible > 0, "and uninterruptible intervals");
  // Runnable-but-not-running is kept as its own state, not folded into a
  // wait: it is CPU contention and has a different cause (E10).
  MPI_CHECK_MSG(runnable > 0, "and preempted-while-runnable intervals");
}

MPI_TEST(det03_leaves_a_background_threads_io_in_the_background,
         {"DET-03", "J14"}) {
  // In this real capture every kernel-confirmed I/O wait is on `WM.task-1`,
  // a WorkManager thread. That is what a background thread is for, so the
  // rule reports nothing -- and says which thread it passed over rather than
  // going quiet.
  auto trace = mapped_fixture();
  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  opts.threshold_overrides.push_back({"DET-03.min_block_ms", 0.1});
  const auto r = rules::analyze(trace, symbols, opts);

  for (const auto& i : r.issues) {
    MPI_CHECK_MSG(i.rule_id != "DET-03",
                  "a background thread's I/O must not be reported");
  }
  const auto* rec = record_for_rule(r, "DET-03");
  MPI_CHECK(rec != nullptr);
  if (rec == nullptr) return;
  MPI_CHECK(rec->outcome == model::RuleOutcome::kRanFoundNothing);
  MPI_CHECK(has_text(rec->skipped_reasons, "WM.task-1"));
  MPI_CHECK(has_text(rec->skipped_reasons,
                     "what a background thread is for"));
}

MPI_TEST(det03_reports_a_main_thread_block_with_its_slice,
         {"DET-03", "H01"}) {
  // Hand-written ftrace, in the real format: the main thread enters a
  // SharedPreferences read, the kernel reports iowait, and it stays
  // uninterruptible for 40 ms. The committed capture has no main-thread I/O
  // wait in it, so the positive path is exercised here rather than by
  // doctoring the real trace.
  const auto parsed = android::parse_atrace(
      "# tracer: nop\n"
      "# entries-in-buffer/entries-written: 12/12   #P:4\n"
      " ut.hutbot.debug-3378 (   3378) [001] ..... 100.000000: "
      "tracing_mark_write: B|3378|SharedPreferencesImpl#loadFromDisk\n"
      " ut.hutbot.debug-3378 (   3378) [001] d..2. 100.001000: "
      "sched_blocked_reason: pid=3378 iowait=1 "
      "caller=folio_wait_bit_common+0x2b0/0x408\n"
      " ut.hutbot.debug-3378 (   3378) [001] d..2. 100.001000: sched_switch: "
      "prev_comm=ut.hutbot.debug prev_pid=3378 prev_prio=110 prev_state=D ==> "
      "next_comm=swapper/1 next_pid=0 next_prio=120\n"
      " <idle>-0 (-------) [001] d..2. 100.041000: sched_switch: "
      "prev_comm=swapper/1 prev_pid=0 prev_prio=120 prev_state=R ==> "
      "next_comm=ut.hutbot.debug next_pid=3378 next_prio=110\n"
      " ut.hutbot.debug-3378 (   3378) [001] ..... 100.045000: "
      "tracing_mark_write: E|3378\n");

  model::NormalizedTrace trace;
  trace.session_id = "det03-main-thread";
  trace.primary_clock_domain = "android.boottime.ns";
  android::map_atrace_to_trace(parsed, {{3378, "ut.hutbot.debug"}}, 3378,
                               "proc", trace);

  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  const auto r = rules::analyze(trace, symbols, opts);

  const model::Issue* found = nullptr;
  for (const auto& i : r.issues) {
    if (i.rule_id == "DET-03") { found = &i; break; }
  }
  MPI_CHECK(found != nullptr);
  if (found == nullptr) return;

  // 40 ms of uninterruptible sleep on the UI thread, with the kernel's own
  // reason: measured, and severity high at that length.
  MPI_CHECK(found->detection_status == model::DetectionStatus::kObserved);
  MPI_CHECK(found->cause_status == model::CauseStatus::kUnknown);
  MPI_CHECK(found->severity == model::Severity::kMedium);
  MPI_CHECK(found->title.find("the UI main thread") != std::string::npos);
  // The app's own slice is the closest thing to a location this provider
  // gives, and it is used when present.
  MPI_CHECK(found->title.find("SharedPreferencesImpl#loadFromDisk") !=
            std::string::npos);
  MPI_CHECK(has_text(found->missing_evidence, "app's own call stack"));
  MPI_CHECK(has_text(found->missing_evidence, "folio_wait_bit_common"));
  MPI_CHECK(has_text(found->proposed_remediation, "off the UI thread"));
  MPI_CHECK_EQ(found->evidence.size(), std::size_t{3});
  MPI_CHECK(!found->metrics.empty());
  if (!found->metrics.empty()) {
    MPI_CHECK_EQ(found->metrics.front().value.value_or(0.0), 40000000.0);
  }
}

MPI_TEST(det03_will_not_promote_a_bare_uninterruptible_state, {"DET-03", "C18"}) {
  // Same trace with the kernel's iowait evidence removed: the `D` states
  // remain, and the rule must refuse to call them I/O.
  auto trace = mapped_fixture();
  std::vector<model::Event> without_io;
  for (const auto& e : trace.events) {
    if (e.category == model::EventCategory::kIo) continue;
    without_io.push_back(e);
  }
  trace.events = std::move(without_io);

  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  const auto r = rules::analyze(trace, symbols, opts);
  const auto* rec = record_for_rule(r, "DET-03");
  MPI_CHECK(rec != nullptr);
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK(has_text(rec->skipped_reasons, "not promoted to an I/O claim"));
  // And it says the collector *did* run, which is not the same as absent.
  MPI_CHECK(has_text(rec->skipped_reasons, "scheduling evidence was collected"));
}

MPI_TEST(det09_reports_waits_on_visible_threads_and_excludes_the_rest,
         {"DET-09", "E06"}) {
  auto trace = mapped_fixture();
  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  opts.threshold_overrides.push_back({"DET-09.min_wait_ms", 1.0});
  const auto r = rules::analyze(trace, symbols, opts);

  std::size_t findings = 0;
  for (const auto& i : r.issues) {
    if (i.rule_id != "DET-09") continue;
    ++findings;
    // The wait is measured; its cause is always qualified.
    MPI_CHECK(i.detection_status == model::DetectionStatus::kObserved);
    MPI_CHECK(i.cause_status == model::CauseStatus::kCandidate ||
              i.cause_status == model::CauseStatus::kUnknown);
    // Never above medium: a qualified finding must not outrank a measured one.
    MPI_CHECK(i.severity != model::Severity::kHigh);
    MPI_CHECK(i.severity_rationale.find("its cause is not") != std::string::npos);
    // Two metrics, and the states are never summed together.
    MPI_CHECK(i.metrics.size() == 2);
    if (!i.metrics.empty()) {
      MPI_CHECK(has_text(i.metrics.front().limitations, "never added to another"));
    }
  }
  MPI_CHECK_MSG(findings > 0, "the main thread does wait in this fixture");

  const auto* rec = record_for_rule(r, "DET-09");
  MPI_CHECK(rec != nullptr);
  // Background threads are excluded with the reason stated, not dropped.
  MPI_CHECK(has_text(rec->skipped_reasons, "not a thread the user waits on"));
}

MPI_TEST(det09_never_names_a_lock_owner, {"DET-09"}) {
  auto trace = mapped_fixture();
  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  opts.threshold_overrides.push_back({"DET-09.min_wait_ms", 1.0});
  const auto r = rules::analyze(trace, symbols, opts);

  for (const auto& i : r.issues) {
    if (i.rule_id != "DET-09") continue;
    // The strongest claim allowed is about the thread that *unblocked* it.
    // Naming a holder would be a fabricated cause.
    MPI_CHECK(i.confidence_basis.find("held") == std::string::npos ||
              i.confidence_basis.find("not necessarily the thread that held") !=
                  std::string::npos);
    if (i.cause_status == model::CauseStatus::kCandidate) {
      MPI_CHECK(has_text(i.missing_evidence, "a lock's owner is a different claim"));
    } else {
      MPI_CHECK(has_text(i.missing_evidence, "not even a candidate can be named"));
    }
  }
}

MPI_TEST(det09_leaves_an_io_wait_to_det03, {"DET-09", "DET-03", "H08"}) {
  // The same block must not be reported twice under two names.
  auto trace = mapped_fixture();
  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  opts.threshold_overrides.push_back({"DET-09.min_wait_ms", 0.1});
  const auto r = rules::analyze(trace, symbols, opts);
  const auto* rec = record_for_rule(r, "DET-09");
  MPI_CHECK(rec != nullptr);
  if (rec == nullptr) return;
  MPI_CHECK(has_text(rec->skipped_reasons, "DET-03 reports"));
}
