// The capture lifecycle and the session package on disk.
//
// These cases were deferred with "needs a collector (M2)" in the requirement
// map. The collector exists now, and the deferral was really about something
// else: none of them needs a *device*, only a collector to drive. So they are
// driven against a fake one -- a test double, which is a different thing from
// synthetic data dressed up as a measurement. Nothing here claims to describe
// a real app.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <thread>

#include "core/report/report.hpp"
#include "core/rules/engine.hpp"
#include "core/session/live_capture.hpp"
#include "core/session/session_store.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

std::string temp_dir(const char* name) {
  const char* base = std::getenv("TMPDIR");
  std::string dir = (base ? std::string(base) : std::string("/tmp/")) +
                    "mpi-test-" + name + "-" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// A collector that streams without a device. It counts what it was asked to
// do, so the lifecycle can be asserted from the collector's side as well as
// the session's.
class FakeCollector final : public session::Collector {
 public:
  std::string id() const override { return "test.fake"; }
  model::Platform platform() const override { return model::Platform::kAndroid; }
  bool supports_streaming() const override { return true; }

  session::CaptureResult capture(const model::DeviceRef&,
                                 const std::vector<model::ProcessInstance>&,
                                 const session::CaptureConfig&,
                                 model::NormalizedTrace&) override {
    session::CaptureResult r;
    r.started = true;
    r.error = "this fake only streams";
    return r;
  }

  session::CaptureResult begin(const model::DeviceRef&,
                               const std::vector<model::ProcessInstance>&,
                               const session::CaptureConfig&,
                               model::NormalizedTrace& out) override {
    ++begins;
    if (begin_delay.count() > 0) {
      std::this_thread::sleep_for(begin_delay);
    }
    if (fail_begin) {
      session::CaptureResult r;
      r.started = false;
      r.error = "the fake was told to refuse";
      return r;
    }
    out.primary_clock_domain = "test.clock.ns";
    session::CaptureResult r;
    r.started = true;
    return r;
  }

  session::LiveUpdate tick(const model::DeviceRef&,
                           const std::vector<model::ProcessInstance>&,
                           const session::CaptureConfig&,
                           model::NormalizedTrace& out) override {
    const auto n = ++ticks;
    session::LiveUpdate u;
    model::FrameRecord f;
    f.event_id = "f" + std::to_string(n);
    f.start_ns = static_cast<model::TimeNs>(n) * 1'000'000;
    f.presented_ns = f.start_ns + 5'000'000;
    f.deadline_ns = 8'000'000;
    f.source = model::FrameSource::kFrameDeadlineReports;
    out.frames.push_back(f);
    out.window_start_ns = 1'000'000;
    out.window_end_ns = f.presented_ns.value_or(f.start_ns);
    u.new_frames = 1;
    u.at_ns = out.window_end_ns;
    return u;
  }

  session::CaptureResult finish(const model::DeviceRef&,
                                const std::vector<model::ProcessInstance>&,
                                const session::CaptureConfig&,
                                model::NormalizedTrace& out) override {
    ++finishes;
    model::Coverage cov;
    cov.collector = "frames";
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = static_cast<std::int64_t>(out.frames.size());
    out.coverage.push_back(cov);
    session::CaptureResult r;
    r.started = true;
    r.any_data = !out.frames.empty();
    return r;
  }

  std::atomic<int> begins{0};
  std::atomic<int> ticks{0};
  std::atomic<int> finishes{0};
  bool fail_begin = false;
  std::chrono::milliseconds begin_delay{0};
};

session::CaptureConfig fast_config() {
  session::CaptureConfig cfg;
  cfg.duration = std::chrono::milliseconds(60'000);  // stopped by hand
  cfg.tick_interval = std::chrono::milliseconds(10);
  return cfg;
}

model::DeviceRef fake_device() {
  model::DeviceRef d;
  d.device_id = "fake-device";
  d.platform = model::Platform::kAndroid;
  d.form = model::DeviceForm::kEmulator;
  return d;
}

std::vector<model::ProcessInstance> one_process() {
  model::ProcessInstance p;
  p.pid = 4242;
  return {p};
}

// Waits for a predicate, so a test never depends on a sleep being long enough.
template <typename Fn>
bool wait_for(Fn&& fn, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return fn();
}

}  // namespace

MPI_TEST(a_capture_starts_ticks_and_stops, {"D01"}) {
  auto collector = std::make_shared<FakeCollector>();
  session::LiveSession live;
  model::NormalizedTrace seed;
  seed.session_id = "s-lifecycle";
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       seed));
  MPI_CHECK(live.running());
  MPI_CHECK(wait_for([&] { return collector->ticks.load() >= 2; }));
  live.stop();
  MPI_CHECK(!live.running());

  const auto snap = live.snapshot();
  MPI_CHECK(snap.state == session::LiveState::kFinished);
  MPI_CHECK(snap.ticks >= 2);
  MPI_CHECK_EQ(collector->begins.load(), 1);
  MPI_CHECK_EQ(collector->finishes.load(), 1);
  // What the ticks produced is in the trace, and the window covers it.
  const auto trace = live.take_trace();
  MPI_CHECK(trace.frames.size() >= 2);
  MPI_CHECK(trace.window_end_ns > trace.window_start_ns);
  MPI_CHECK(!trace.coverage.empty());
}

MPI_TEST(a_snapshot_before_the_close_is_preliminary, {"D01", "H18"}) {
  auto collector = std::make_shared<FakeCollector>();
  session::LiveSession live;
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  MPI_CHECK(wait_for([&] { return collector->ticks.load() >= 1; }));
  const auto during = live.snapshot();
  MPI_CHECK_MSG(during.analysis_is_preliminary,
                "analysis read while the window is open is preliminary: a "
                "detector that has found nothing yet may still fire");
  live.stop();
  const auto after = live.snapshot();
  MPI_CHECK_MSG(!after.analysis_is_preliminary,
                "once the window is closed the analysis is final");
}

MPI_TEST(stopping_during_startup_leaves_no_half_state, {"D02"}) {
  // A stop that arrives while the collector is still preparing the device.
  // The session must end in a state that says what happened, not sit in
  // `starting` forever and not report a capture it never made.
  auto collector = std::make_shared<FakeCollector>();
  collector->begin_delay = std::chrono::milliseconds(300);
  session::LiveSession live;
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  live.stop();
  MPI_CHECK(!live.running());
  const auto snap = live.snapshot();
  MPI_CHECK_MSG(snap.state == session::LiveState::kFinished ||
                    snap.state == session::LiveState::kFailed,
                "the session settles rather than staying in `starting`");
  // begin() ran; whether any tick did is timing, but the collector must have
  // been closed either way.
  MPI_CHECK_EQ(collector->begins.load(), 1);
}

MPI_TEST(stopping_twice_is_idempotent, {"D03"}) {
  auto collector = std::make_shared<FakeCollector>();
  session::LiveSession live;
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  MPI_CHECK(wait_for([&] { return collector->ticks.load() >= 1; }));
  live.stop();
  const auto first = live.snapshot();
  live.stop();
  live.stop();
  const auto third = live.snapshot();
  MPI_CHECK(!live.running());
  MPI_CHECK(third.state == first.state);
  MPI_CHECK_EQ(third.ticks, first.ticks);
  MPI_CHECK_MSG(collector->finishes.load() == 1,
                "the collector is closed once, however many times stop() is "
                "called: a second finish would append a second coverage row "
                "for the same window");
}

MPI_TEST(a_second_capture_on_a_running_session_is_refused, {"D21"}) {
  // Two collectors against one target at once is the conflicting-collector
  // case: the second must be refused rather than interleaving its ticks with
  // the first's into the same trace.
  auto first = std::make_shared<FakeCollector>();
  auto second = std::make_shared<FakeCollector>();
  session::LiveSession live;
  MPI_CHECK(live.start(first, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  MPI_CHECK(wait_for([&] { return first->ticks.load() >= 1; }));
  const bool accepted = live.start(second, fake_device(), one_process(),
                                   fast_config(), model::NormalizedTrace{});
  MPI_CHECK_MSG(!accepted, "a second start is refused while one is running");
  MPI_CHECK_EQ(second->begins.load(), 0);
  MPI_CHECK(live.running());
  live.stop();
  MPI_CHECK_EQ(first->finishes.load(), 1);
}

MPI_TEST(a_collector_that_refuses_to_begin_writes_nothing, {"D01", "H01"}) {
  auto collector = std::make_shared<FakeCollector>();
  collector->fail_begin = true;
  session::LiveSession live;
  const bool started = live.start(collector, fake_device(), one_process(),
                                  fast_config(), model::NormalizedTrace{});
  MPI_CHECK(!started);
  const auto snap = live.snapshot();
  MPI_CHECK(snap.state == session::LiveState::kFailed);
  MPI_CHECK(snap.error.find("told to refuse") != std::string::npos);
  MPI_CHECK_EQ(collector->ticks.load(), 0);
  MPI_CHECK_MSG(live.take_trace().frames.empty(),
                "a refused start produces no events to mistake for a capture");
}

// --- the package on disk ----------------------------------------------------

MPI_TEST(a_written_package_reopens_with_no_device, {"H13", "J07"}) {
  // Offline reopen: the package is self-describing, so reading it needs
  // nothing that produced it.
  const auto dir = temp_dir("reopen");
  session::SessionManifest manifest;
  manifest.session_id = "s-reopen-001";
  manifest.created_at = "2026-09-15T00:00:00Z";
  manifest.tool_version = "test";
  manifest.state = session::SessionState::kCompleted;

  model::NormalizedTrace trace;
  trace.session_id = manifest.session_id;
  trace.primary_clock_domain = "test.clock.ns";
  trace.window_start_ns = 1000;
  trace.window_end_ns = 2000;
  model::AnalysisResult analysis;
  analysis.session_id = manifest.session_id;
  model::DiscoverySnapshot snap;

  const auto written = session::write_package(dir, manifest, trace, analysis,
                                              snap, "# report\n", "{}\n");
  MPI_CHECK_MSG(written.ok, written.error);
  if (!written.ok) return;

  const auto loaded = session::load_package(written.package_dir);
  MPI_CHECK_MSG(loaded.ok, loaded.error);
  MPI_CHECK_EQ(loaded.manifest.session_id, std::string("s-reopen-001"));
  MPI_CHECK(loaded.checksum_failures.empty());
  MPI_CHECK(!loaded.trace_path.empty());
  MPI_CHECK_MSG(loaded.heap_path.empty(),
                "no heap dump was stored, and an empty path means exactly "
                "that rather than one not being looked for");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_stored_artifact_is_checksummed_and_found_again, {"H13", "DET-06"}) {
  const auto dir = temp_dir("artifact");
  const auto source = dir + "/incoming.hprof";
  {
    std::ofstream f(source, std::ios::binary);
    f << "JAVA PROFILE 1.0.3";
  }

  session::SessionManifest manifest;
  manifest.session_id = "s-artifact-001";
  manifest.tool_version = "test";
  manifest.state = session::SessionState::kCompleted;
  model::NormalizedTrace trace;
  trace.session_id = manifest.session_id;
  model::AnalysisResult analysis;
  model::DiscoverySnapshot snap;

  const auto written = session::write_package(
      dir + "/sessions", manifest, trace, analysis, snap, "", "{}",
      {{"heap.hprof", source}});
  MPI_CHECK_MSG(written.ok, written.error);
  if (!written.ok) return;

  const auto loaded = session::load_package(written.package_dir);
  MPI_CHECK(loaded.ok);
  MPI_CHECK_MSG(!loaded.heap_path.empty(),
                "a stored dump is found on reopen without being named again");
  MPI_CHECK(loaded.checksum_failures.empty());

  // And a corrupted artifact is reported rather than read as though intact.
  {
    std::ofstream f(loaded.heap_path, std::ios::binary | std::ios::trunc);
    f << "tampered";
  }
  const auto again = session::load_package(written.package_dir);
  MPI_CHECK(again.ok);
  MPI_CHECK_MSG(!again.checksum_failures.empty(),
                "a changed artifact fails its checksum: the package reports "
                "it and leaves the decision to the operator");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_missing_artifact_fails_the_write_rather_than_the_manifest,
         {"H13", "D06"}) {
  // A manifest listing a file that is not there is worse than a failed write:
  // the package would look complete and fail only when someone tried to read
  // the evidence.
  const auto dir = temp_dir("missing-artifact");
  session::SessionManifest manifest;
  manifest.session_id = "s-missing-001";
  manifest.tool_version = "test";
  model::NormalizedTrace trace;
  model::AnalysisResult analysis;
  model::DiscoverySnapshot snap;
  const auto written = session::write_package(
      dir, manifest, trace, analysis, snap, "", "{}",
      {{"heap.hprof", dir + "/does-not-exist.hprof"}});
  MPI_CHECK(!written.ok);
  MPI_CHECK(written.error.find("cannot read the artifact") != std::string::npos);
  // And nothing half-written is left behind.
  MPI_CHECK_MSG(!std::filesystem::exists(dir + "/s-missing-001"),
                "a failed write leaves no package");
  MPI_CHECK_MSG(!std::filesystem::exists(dir + "/s-missing-001.partial"),
                "and no partial directory either");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(delete_refuses_a_directory_that_is_not_the_named_session,
         {"J08", "D22"}) {
  // The destructive path, tested in the one way that is safe to automate:
  // every refusal, and one deletion of a package this test wrote itself.
  const auto dir = temp_dir("delete");
  session::SessionManifest manifest;
  manifest.session_id = "s-delete-001";
  manifest.tool_version = "test";
  manifest.state = session::SessionState::kCompleted;
  model::NormalizedTrace trace;
  trace.session_id = manifest.session_id;
  model::AnalysisResult analysis;
  model::DiscoverySnapshot snap;
  const auto written = session::write_package(dir, manifest, trace, analysis,
                                              snap, "", "{}");
  MPI_CHECK(written.ok);
  if (!written.ok) return;

  // A directory with no manifest: refused, whatever it is.
  const auto plain = dir + "/not-a-session";
  std::error_code ec;
  std::filesystem::create_directories(plain, ec);
  {
    std::ofstream f(plain + "/important.txt");
    f << "not ours";
  }
  const auto refused = session::delete_package(plain, "s-delete-001");
  MPI_CHECK(!refused.ok);
  MPI_CHECK(refused.removed.empty());
  MPI_CHECK_MSG(std::filesystem::exists(plain + "/important.txt"),
                "the refusal removed nothing");

  // The right directory but the wrong id: also refused, because a mistyped
  // argument is exactly how the wrong session gets deleted.
  const auto wrong_id =
      session::delete_package(written.package_dir, "s-some-other-session");
  MPI_CHECK(!wrong_id.ok);
  MPI_CHECK(wrong_id.removed.empty());
  MPI_CHECK(std::filesystem::exists(written.package_dir));

  // And the correct pair works, removing only what the manifest listed.
  const auto done =
      session::delete_package(written.package_dir, "s-delete-001");
  MPI_CHECK_MSG(done.ok, done.error);
  MPI_CHECK(!done.removed.empty());
  MPI_CHECK(!std::filesystem::exists(written.package_dir));
  MPI_CHECK_MSG(std::filesystem::exists(plain + "/important.txt"),
                "the sibling directory is untouched");
  std::filesystem::remove_all(dir, ec);
}
