// Android live capture over Android Platform Tools.
//
// Deliberately built on the *text* interfaces rather than on Perfetto's
// protobuf output. Two reasons, both recorded in ADR-0006:
//
//  * Perfetto writes protobuf. Consuming it needs a protobuf runtime, which
//    would end the empty third-party licence inventory that ADR-0002 exists to
//    protect, for data these three sources already provide.
//  * `dumpsys gfxinfo <pkg> framestats` is the canonical Android frame-timing
//    source and reports a *provider-supplied deadline* per frame, which is
//    exactly what DET-01 needs and strictly better than a derived one.
//
// Sources, each independently probed and independently able to fail:
//
//  | Source                          | Yields                                |
//  |---------------------------------|---------------------------------------|
//  | dumpsys gfxinfo PKG framestats  | FrameRecord with a real deadline       |
//  | simpleperf record + report-sample | CpuSample with symbolised callchains |
//  | dumpsys meminfo PKG             | CounterSeries per memory family        |
//
// simpleperf is invoked with `--app`, which routes through run-as and so works
// only on a debuggable or profileable package. That is Android's own model and
// this adapter does not attempt to work around it: on a non-profileable target
// the CPU source reports `permission_required` with the setup that would fix
// it.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adapters/android/adb_adapter.hpp"
#include "core/session/collector.hpp"

namespace mpi::android {

// One row of the framestats CSV. Field names match the CSV header so the
// mapping to the normalized model stays auditable against real output.
struct FrameStatsRow {
  std::int64_t flags = 0;
  std::int64_t intended_vsync = 0;
  std::int64_t vsync = 0;
  std::int64_t frame_deadline = 0;
  std::int64_t frame_completed = 0;
  std::int64_t gpu_completed = 0;
  std::int64_t display_present_time = 0;
  std::int64_t frame_interval = 0;
  std::int64_t draw_start = 0;
};

// Parses the `---PROFILEDATA---` blocks of `dumpsys gfxinfo PKG framestats`.
// Reads the column order from the header rather than assuming it, because the
// column set has changed across Android releases.
struct FrameStats {
  std::vector<FrameStatsRow> rows;
  std::vector<std::string> warnings;
  // Distinct FrameInterval values observed, which give the refresh rate
  // without having to ask for it separately.
  std::vector<std::int64_t> observed_frame_intervals;
};
FrameStats parse_gfxinfo_framestats(const std::string& text);

// Parses `simpleperf report-sample --show-callchain`.
struct SimpleperfSamples {
  struct Sample {
    std::int64_t time_ns = 0;
    std::int64_t event_count = 0;  // sample weight
    std::int32_t thread_id = 0;
    std::string thread_name;
    // Outermost first, after reversing simpleperf's leaf-first order.
    std::vector<std::string> frames;
  };
  std::vector<Sample> samples;
  // From the meta_info block: these are real build facts read off the device.
  std::string event_type;
  std::string app_package_name;
  std::string app_type;       // "debuggable" / "profileable" / ...
  std::string build_type;     // "user" / "userdebug" / "eng"
  std::optional<int> sdk_version;
  std::vector<std::string> warnings;
};
SimpleperfSamples parse_simpleperf_report_sample(const std::string& text);

// Parses `dumpsys meminfo PKG`. Only the families the specification requires
// to stay distinct are extracted; they are never summed together.
struct MemInfo {
  std::optional<double> pss_total_bytes;
  std::optional<double> rss_total_bytes;
  std::optional<double> private_dirty_bytes;
  std::optional<double> native_heap_rss_bytes;
  std::optional<double> dalvik_heap_rss_bytes;
  std::optional<std::int32_t> pid;
};
MemInfo parse_dumpsys_meminfo(const std::string& text);

// What `am start -W` reported about one launch.
//
// `TotalTime: 0` is the trap here. The platform prints it when no activity
// was started at all -- re-launching an app that is already foreground prints
// "Warning: Activity not started, intent has been delivered to currently
// running top-most instance" and a TotalTime of zero. Read literally that is
// a zero-millisecond startup, comfortably inside any budget. So the zero is
// never carried as a measurement: `started` says whether a launch happened,
// and the durations stay absent when it did not.
struct AmStartResult {
  bool started = false;
  // "ok" / "error ..." exactly as the platform reported it.
  std::string status;
  // COLD / WARM / HOT / UNKNOWN, as the platform classified it. Never
  // inferred from the durations.
  std::string launch_state;
  std::string component;
  // Absent unless the platform printed a figure for an actual launch.
  std::optional<model::TimeNs> total_time_ns;
  std::optional<model::TimeNs> wait_time_ns;
  // Warnings the platform emitted, kept verbatim.
  std::vector<std::string> warnings;
  std::string refusal;  // why no measurement was produced
};
AmStartResult parse_am_start_w(const std::string& text);

// `ActivityTaskManager: Displayed <component> for user N: +3s687ms`.
//
// This is the platform's own first-frame figure. It is a different endpoint
// from `am start -W`'s TotalTime and is kept separate rather than averaged
// with it.
struct DisplayedRecord {
  std::string component;
  model::TimeNs elapsed_ns = 0;
  std::string raw;  // the duration as printed, e.g. "+3s687ms"
};
// Returns every Displayed line in the log, oldest first.
std::vector<DisplayedRecord> parse_displayed_log(const std::string& text);

// The launcher component from `cmd package resolve-activity --brief PKG`,
// whose last line is `package/.Activity`. Empty when the package declares no
// launchable activity, which is a real answer and not an error.
std::string parse_resolved_activity(const std::string& text);

// Reads the leading decimal number of a string, as in /proc/uptime's first
// field. Returns false on anything it cannot read, so a boot time is never
// silently defaulted -- a zero there would anchor a whole capture to the wrong
// instant. Exposed for testing.
bool parse_leading_double(const std::string& text, double& out);

class AdbCollector final : public session::Collector {
 public:
  // Defaults to the resolver, not the bare name: a Dock-launched app has
  // no SDK on PATH, and the capture then failed per source with "adb: No
  // such file or directory" while discovery had already found the device.
  explicit AdbCollector(std::string adb_path = default_adb_path());

  std::string id() const override { return "android.adb.text-sources"; }
  model::Platform platform() const override { return model::Platform::kAndroid; }

  session::CaptureResult capture(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

  // ---- streaming ----------------------------------------------------------
  //
  // Streaming suits these sources well, and in one case is strictly better:
  //
  //  * `framestats` returns a ring buffer of roughly the last 120 frames on
  //    every read, so consecutive reads overlap. Frames are de-duplicated by
  //    IntendedVsync, which makes a read idempotent and a tick's delta the
  //    frames that are genuinely new.
  //  * `meminfo` per tick turns a single instantaneous reading into a real
  //    time series, which is what DET-05 will eventually need.
  //  * `simpleperf` is run in a short window per tick. That costs a process
  //    spawn each time, which LiveUpdate::tick_cost reports.
  bool supports_streaming() const override { return true; }

  // The device clock, from the anchor taken at capture start.
  std::optional<session::Collector::DeviceClock> device_clock_at(
      std::chrono::steady_clock::time_point host_instant) const override;

  // Launches the app and records the platform's own startup figures.
  session::Collector::LaunchReport launch(const model::DeviceRef& device,
                                          const std::string& app_identifier,
                                          const session::CaptureConfig& config,
                                          model::NormalizedTrace& out) override;

  session::CaptureResult begin(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

  session::LiveUpdate tick(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

  session::CaptureResult finish(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

 private:
  // Scheduling and I/O evidence for the target's own threads.
  //
  // atrace is system-wide, so this needs the app's thread ids to attribute
  // anything: everything else in the file belongs to other processes and is
  // counted as out of scope rather than folded into the app's totals.
  std::int64_t collect_scheduling(const model::DeviceRef& device,
                                  const session::CaptureConfig& config,
                                  std::int32_t pid, model::NormalizedTrace& out,
                                  model::Capability& capability);

  // A heap dump, pulled to `config.artifact_dir`.
  //
  // Returns the local path, or empty with the reason on `capability`. The file
  // is not parsed here: parsing is analysis, and a collector that parsed its
  // own output would make a capture fail for a reason that has nothing to do
  // with the device.
  // The process's own CPU time, from /proc/<pid>/stat. Sampled on the memory
  // tick so the two describe the same instant.
  std::int64_t collect_cpu_time_once(const model::DeviceRef& device,
                                     const session::CaptureConfig& config,
                                     model::TimeNs at_ns,
                                     model::NormalizedTrace& out);

  std::string collect_heap_dump(const model::DeviceRef& device,
                                const session::CaptureConfig& config,
                                std::int32_t pid,
                                model::Capability& capability);

  std::string adb_path_;
  std::vector<std::string> shell_argv(const std::string& serial,
                                      const std::vector<std::string>& args) const;

  // Per-session streaming state. A collector instance drives one capture at a
  // time, which the live session guarantees.
  //
  // CPU sampling runs on its own thread and parks its results in
  // `pending_samples`; the tick loop drains them into the trace. That keeps
  // all trace mutation on the tick thread, so the live session's lock remains
  // the only thing protecting the trace.
  struct StreamState {
    std::string package;
    std::string process_key;
    std::string clock_id;
    // The target's pid, for the per-process reads that need one.
    std::int32_t pid = 0;
    // The device's CLK_TCK: 0 = not read yet, -1 = refused. Never defaulted
    // to 100, however common that is -- a wrong constant would make every
    // CPU-time figure wrong by a constant factor and look plausible.
    std::int64_t clk_tck = 0;
    bool clk_tck_refused = false;
    // How many CPUs the device reports, for the ceiling a per-core
    // utilisation is read against: 0 = not read yet, -1 = unreadable. Never
    // defaulted, for the same reason as CLK_TCK.
    std::int64_t core_count = 0;
    // The previous CPU-time reading, so a utilisation can be a difference of
    // two measurements. Absent until the second reading of a capture, and a
    // rate is published only once it exists -- 0% on the first tick would
    // read as an idle app at the one moment it certainly is not.
    std::optional<double> last_cpu_ns;
    std::optional<model::TimeNs> last_cpu_at_ns;
    bool frames_reset = false;
    bool frames_ok = true;
    bool cpu_ok = true;
    bool memory_ok = true;
    std::string cpu_error;
    std::int64_t frame_seq = 0;
    // Highest IntendedVsync already recorded.
    //
    // `framestats` does NOT drain when read: it is a ring buffer of roughly
    // the last 120 frames, returned in full on every read. Consecutive reads
    // therefore overlap heavily, and appending each read's rows wholesale
    // double-counts frames -- measured at 523 frames across 8 ticks where
    // about 120 had actually rendered. IntendedVsync is unique and monotonic
    // per frame, so it is the identity that makes a read idempotent.
    model::TimeNs last_frame_vsync = 0;
    std::int64_t frames_seen_duplicate = 0;
    // Stretches where the ring buffer wrapped between two ticks. The frames
    // in them rendered and were never read, so they are missing evidence, not
    // an idle app -- and only a coverage gap says that.
    std::vector<model::CoverageGap> frame_gaps;
    std::int64_t sample_seq = 0;
    std::int64_t tick_count = 0;
    std::chrono::milliseconds total_tick_cost{0};
    model::TimeNs window_lo = 0;
    model::TimeNs window_hi = 0;
    bool have_window = false;
    // Counter series are created once and appended to, so memory becomes a
    // series rather than a set of one-point series.
    std::vector<std::string> counter_names;

    // ---- device clock anchor ----
    //
    // `dumpsys meminfo` reports no timestamp of its own, so a memory sample
    // has to be placed on the device clock by the host. The device's boot
    // time is read once at capture start and paired with the host's steady
    // clock; each later tick is that base plus the host elapsed time.
    //
    // Borrowing the timestamp of the newest frame or CPU sample instead --
    // which is what this did first -- stamps a memory reading with the time
    // of an unrelated event, and stamps it 0 until the first such event
    // arrives. Since the CPU worker needs about 5.6 s for its first window,
    // the opening samples of every capture landed at 0.
    model::TimeNs boot_base_ns = 0;
    std::chrono::steady_clock::time_point host_base{};
    bool clock_anchored = false;

    // Resolution of /proc/uptime, which reports hundredths of a second.
    static constexpr model::TimeNs kUptimeResolutionNs = 10'000'000;

    // The device clock at this instant, by extrapolation from the anchor.
    // Only meaningful when `clock_anchored`.
    model::TimeNs device_now_ns() const {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - host_base)
                               .count();
      return boot_base_ns + static_cast<model::TimeNs>(elapsed);
    }

    // ---- CPU worker ----
    std::thread cpu_thread;
    std::atomic<bool> cpu_stop{false};
    std::mutex cpu_mutex;
    std::vector<SimpleperfSamples::Sample> pending_samples;
    SimpleperfSamples pending_meta;
    bool meta_recorded = false;
    std::int64_t cpu_windows_done = 0;
    std::chrono::milliseconds cpu_busy{0};
    std::string cpu_status_error;
    bool cpu_denied = false;
    // Windows the sampler actually covered, so the intervals between them can
    // be reported as gaps instead of looking like measured idle time.
    std::vector<std::pair<model::TimeNs, model::TimeNs>> cpu_covered;

    // Clears everything for a new session. Not an assignment, because the
    // thread and mutex members are not assignable -- and a stale worker must
    // be joined before its state is discarded, which begin() does.
    void reset() {
      package.clear();
      process_key.clear();
      clock_id.clear();
      pid = 0;
      clk_tck = 0;
      clk_tck_refused = false;
      core_count = 0;
      last_cpu_ns.reset();
      last_cpu_at_ns.reset();
      frames_reset = false;
      frames_ok = true;
      cpu_ok = true;
      memory_ok = true;
      cpu_error.clear();
      frame_seq = 0;
      last_frame_vsync = 0;
      frames_seen_duplicate = 0;
      frame_gaps.clear();
      sample_seq = 0;
      tick_count = 0;
      total_tick_cost = std::chrono::milliseconds{0};
      window_lo = 0;
      window_hi = 0;
      have_window = false;
      counter_names.clear();
      boot_base_ns = 0;
      host_base = {};
      clock_anchored = false;
      cpu_stop.store(false);
      pending_samples.clear();
      pending_meta = SimpleperfSamples{};
      meta_recorded = false;
      cpu_windows_done = 0;
      cpu_busy = std::chrono::milliseconds{0};
      cpu_status_error.clear();
      cpu_denied = false;
      cpu_covered.clear();
    }
  };
  StreamState stream_;

  // Shared by the batch and streaming paths so both produce identical records.
  std::int64_t collect_frames_once(const model::DeviceRef& device,
                                   const session::CaptureConfig& config,
                                   model::NormalizedTrace& out,
                                   std::vector<std::string>& notes,
                                   bool& source_ok, std::string& evidence);
  void extend_window_from_trace(model::NormalizedTrace& out);
  std::int64_t collect_memory_once(const model::DeviceRef& device,
                                   const session::CaptureConfig& config,
                                   model::TimeNs at_ns,
                                   model::NormalizedTrace& out,
                                   bool& source_ok, std::string& evidence);
  // Records and symbolises one CPU window, returning the parsed samples
  // rather than writing them into the trace: it runs on the worker thread.
  SimpleperfSamples record_cpu_window(const model::DeviceRef& device,
                                      const session::CaptureConfig& config,
                                      std::chrono::milliseconds window,
                                      bool& ok, std::string& error);
  // Moves whatever the worker has parked into the trace. Tick thread only.
  std::int64_t drain_cpu_samples(model::NormalizedTrace& out);
  void start_cpu_worker(const model::DeviceRef& device,
                        const session::CaptureConfig& config);
  void stop_cpu_worker();
};

}  // namespace mpi::android
