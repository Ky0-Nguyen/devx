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
#include "core/session/suppressions.hpp"
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
    const int die = die_after.load();
    if (die >= 0 && n > die) {
      // Nothing collected, and every source reporting failure: the shape of
      // a tick against a device that is no longer answering.
      model::Capability c;
      c.id = "test.frames";
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "the fake device stopped answering";
      u.source_status.push_back(std::move(c));
      return u;
    }
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
  // After this many ticks, every source reports failure and nothing is
  // collected -- what a capture sees when the device goes away.
  std::atomic<int> die_after{-1};
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

// --- the project's suppression list -----------------------------------------

MPI_TEST(a_suppression_with_no_reason_is_refused_on_read_and_write,
         {"H10", "H14"}) {
  // The one rule worth refusing on. A suppression nobody can review is
  // permanent by accident, so it never reaches the engine -- and it is
  // refused out loud, because one that vanished quietly would look like a
  // finding that was never suppressed.
  const auto dir = temp_dir("suppress-reason");
  const auto path = dir + "/suppressions.json";
  {
    std::ofstream f(path);
    f << R"({"schema_version":"2.0","suppressions":[
        {"rule_id":"DET-01","reason":"tracked in TICKET-9","author":"a"},
        {"rule_id":"DET-02"},
        {"reason":"a reason with no rule"},
        "not an object"
      ]})";
  }
  session::SuppressionFile file;
  const auto read = session::read_suppressions(path, file);
  MPI_CHECK_MSG(read.ok, read.error);
  MPI_CHECK_EQ(file.entries.size(), std::size_t{1});
  MPI_CHECK_EQ(read.rejected.size(), std::size_t{3});
  bool named = false;
  for (const auto& r : read.rejected) {
    if (r.find("DET-02") != std::string::npos &&
        r.find("not auditable") != std::string::npos) {
      named = true;
    }
  }
  MPI_CHECK(named);

  // And the same rule on the way out.
  session::SuppressionFile bad;
  session::SuppressionEntry e;
  e.rule_id = "DET-03";
  const auto wrote = session::write_suppressions(path, bad = [&] {
    session::SuppressionFile f;
    f.entries.push_back(e);
    return f;
  }());
  MPI_CHECK(!wrote.ok);
  MPI_CHECK(wrote.error.find("no reason") != std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_missing_suppression_file_is_not_an_error, {"H14"}) {
  // A project with no suppressions is the normal case, and the normal case is
  // not an error. Returning one would make every analysis of a clean project
  // fail.
  session::SuppressionFile file;
  const auto read = session::read_suppressions("/nonexistent/suppressions.json",
                                               file);
  MPI_CHECK(read.ok);
  MPI_CHECK(read.error.empty());
  MPI_CHECK(file.entries.empty());
  MPI_CHECK(read.rejected.empty());
}

MPI_TEST(a_suppression_list_round_trips_with_everything_that_audits_it,
         {"H10", "H14"}) {
  const auto dir = temp_dir("suppress-round");
  const auto path = dir + "/suppressions.json";
  session::SuppressionFile file;
  session::SuppressionEntry e;
  e.rule_id = "DET-04";
  e.fingerprint = "DET-04-abc";
  e.reason = "third-party thread, accepted";
  e.expiry = "2026-12-31";
  e.author = "someone";
  e.created_at = "2026-09-15T00:00:00Z";
  e.reference = "TICKET-7";
  file.entries.push_back(e);
  MPI_CHECK(session::write_suppressions(path, file).ok);

  session::SuppressionFile back;
  MPI_CHECK(session::read_suppressions(path, back).ok);
  MPI_CHECK_EQ(back.entries.size(), std::size_t{1});
  if (back.entries.empty()) return;
  const auto& r = back.entries.front();
  MPI_CHECK_EQ(r.rule_id, e.rule_id);
  MPI_CHECK_EQ(r.fingerprint, e.fingerprint);
  MPI_CHECK_EQ(r.reason, e.reason);
  MPI_CHECK_EQ(r.expiry, e.expiry);
  MPI_CHECK_EQ(r.author, e.author);
  MPI_CHECK_MSG(r.reference == e.reference,
                "the reference survives: a reason wants somewhere to point");
  MPI_CHECK_MSG(r.created_at == e.created_at,
                "and when it was made, which is what dates an expiry");

  // The engine form carries what the report needs to stay auditable.
  const auto engine = back.to_engine();
  MPI_CHECK_EQ(engine.size(), std::size_t{1});
  if (engine.empty()) return;
  MPI_CHECK_EQ(engine.front().reason, e.reason);
  MPI_CHECK_EQ(engine.front().expiry, e.expiry);
  MPI_CHECK_EQ(engine.front().author, e.author);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(an_empty_fingerprint_suppresses_the_whole_rule, {"H14"}) {
  // A much larger claim than suppressing one finding, and the difference has
  // to survive the file: an entry with no fingerprint covers every finding
  // the rule produces, now and later.
  const auto dir = temp_dir("suppress-wide");
  const auto path = dir + "/suppressions.json";
  session::SuppressionFile file;
  session::SuppressionEntry e;
  e.rule_id = "DET-10";
  e.reason = "render patterns are reviewed separately";
  file.entries.push_back(e);
  MPI_CHECK(session::write_suppressions(path, file).ok);
  session::SuppressionFile back;
  MPI_CHECK(session::read_suppressions(path, back).ok);
  MPI_CHECK_EQ(back.entries.size(), std::size_t{1});
  if (back.entries.empty()) return;
  MPI_CHECK(back.entries.front().fingerprint.empty());
  MPI_CHECK(back.to_engine().front().fingerprint.empty());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_malformed_suppression_file_is_an_error_not_an_empty_list,
         {"H14", "D18"}) {
  // Silently reading a broken file as "nothing is suppressed" would apply no
  // suppressions and say nothing, which is the same shape of failure as
  // applying them all.
  const auto dir = temp_dir("suppress-malformed");
  const auto path = dir + "/suppressions.json";
  {
    std::ofstream f(path);
    f << "{ this is not json";
  }
  session::SuppressionFile file;
  const auto read = session::read_suppressions(path, file);
  MPI_CHECK(!read.ok);
  MPI_CHECK(!read.error.empty());

  {
    std::ofstream f(path, std::ios::trunc);
    f << R"({"schema_version":"2.0"})";
  }
  const auto no_array = session::read_suppressions(path, file);
  MPI_CHECK(!no_array.ok);
  MPI_CHECK(no_array.error.find("suppressions") != std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_package_from_another_schema_version_is_named_not_migrated,
         {"J13"}) {
  // There is one schema version, so there is nothing to migrate *to*, and
  // claiming a migration would be worse than admitting there is none. What
  // the loader owes a reader is recognition: a package written against a
  // version this build does not know must be reported, because a field it
  // cannot interpret would otherwise read as a field the capture did not
  // have.
  const auto dir = temp_dir("schema");
  session::SessionManifest manifest;
  manifest.session_id = "s-schema-001";
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

  // As written, nothing to say.
  const auto current = session::load_package(written.package_dir);
  MPI_CHECK(current.ok);
  for (const auto& n : current.notes) {
    MPI_CHECK_MSG(n.find("schema version") == std::string::npos,
                  "a current package needs no schema note: " + n);
  }

  // Rewrite the manifest as an older version, the way a package from a
  // previous build would arrive.
  const auto manifest_path = written.package_dir + "/manifest.json";
  std::string text;
  {
    std::ifstream in(manifest_path);
    text.assign((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
  }
  const auto at = text.find("\"2.0\"");
  MPI_CHECK(at != std::string::npos);
  if (at == std::string::npos) return;
  text.replace(at, 5, "\"1.3\"");
  {
    std::ofstream out(manifest_path, std::ios::trunc);
    out << text;
  }

  const auto older = session::load_package(written.package_dir);
  // Readable, because refusing would strand a package whose events are
  // probably fine -- but never silent.
  MPI_CHECK_MSG(older.ok, "an older package still loads: " + older.error);
  MPI_CHECK_EQ(older.manifest.schema_version, std::string("1.3"));
  bool named = false;
  for (const auto& n : older.notes) {
    if (n.find("1.3") != std::string::npos &&
        n.find("no migration") != std::string::npos) {
      named = true;
    }
  }
  MPI_CHECK_MSG(named,
                "the version is named and the absence of a migration stated");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(an_interrupted_write_is_never_mistaken_for_a_session, {"J13", "D06"}) {
  // The recovery half. A package is built in a sibling `.partial` directory
  // and renamed into place, so a write that dies halfway leaves something
  // that is not a session rather than one that looks finished and is not.
  const auto dir = temp_dir("partial");
  const auto half = dir + "/s-partial-001.partial";
  std::error_code ec;
  std::filesystem::create_directories(half + "/raw", ec);
  {
    std::ofstream f(half + "/manifest.json");
    f << R"({"schema_version":"2.0","session_id":"s-partial-001"})";
  }
  // Loading the partial directory by name fails: its id does not match the
  // directory a session lives in, and nothing about it claims to be
  // complete.
  const auto loaded = session::load_package(half);
  if (loaded.ok) {
    // If it parses at all, it must not claim to be a completed session.
    MPI_CHECK_MSG(loaded.manifest.state != session::SessionState::kCompleted,
                  "a half-written package must never read as completed");
  }
  // And the session's real directory does not exist, so a lister cannot
  // offer it.
  MPI_CHECK(!std::filesystem::exists(dir + "/s-partial-001"));
  std::filesystem::remove_all(dir, ec);
}

MPI_TEST(a_capture_that_loses_its_device_says_so_and_keeps_what_it_had,
         {"D20", "H09"}) {
  // The run loop used to ignore a tick's outcome entirely, so a device that
  // went away mid-capture produced a session reporting `partial: false` with
  // no reason -- data for part of the window, nothing explaining the rest,
  // and no way for a reader to tell that from an app that went quiet.
  //
  // Both halves of D20 matter: what was collected before the loss is kept
  // and is real, and the loss itself is stated.
  auto collector = std::make_shared<FakeCollector>();
  collector->die_after.store(2);
  session::LiveSession live;
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  MPI_CHECK(wait_for([&] { return collector->ticks.load() >= 8; }));
  live.stop();

  const auto trace = live.take_trace();
  MPI_CHECK_MSG(trace.frames.size() == 2,
                "the two good ticks' data is kept: got " +
                    std::to_string(trace.frames.size()));
  MPI_CHECK_MSG(trace.partial,
                "a capture that lost its device is partial, not complete");
  bool stated = false;
  for (const auto& r : trace.partial_reasons) {
    if (r.find("stopped answering") != std::string::npos) stated = true;
    // And the distinction that matters to a reader.
    if (r.find("not a quiet app") != std::string::npos) stated = stated && true;
  }
  MPI_CHECK_MSG(stated, "the reason names the lost connection");
  bool distinguishes = false;
  for (const auto& r : trace.partial_reasons) {
    if (r.find("not a quiet app") != std::string::npos) distinguishes = true;
  }
  MPI_CHECK_MSG(distinguishes,
                "and says the silence after it is a lost device rather than "
                "an idle app");
}

MPI_TEST(a_healthy_capture_is_never_called_partial, {"D20", "H09"}) {
  // The false positive that would make the flag worthless. A tick that
  // collects nothing but reports a working source is a quiet moment, not a
  // lost device -- verified against the real emulator too, where killing the
  // adb server turned out not to be a disconnect at all: the client respawns
  // the server, the ticks keep succeeding, and nothing was marked partial.
  auto collector = std::make_shared<FakeCollector>();
  session::LiveSession live;
  MPI_CHECK(live.start(collector, fake_device(), one_process(), fast_config(),
                       model::NormalizedTrace{}));
  MPI_CHECK(wait_for([&] { return collector->ticks.load() >= 6; }));
  live.stop();
  const auto trace = live.take_trace();
  MPI_CHECK(!trace.partial);
  MPI_CHECK(trace.partial_reasons.empty());
}
