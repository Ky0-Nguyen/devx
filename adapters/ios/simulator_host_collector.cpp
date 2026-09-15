#include "adapters/ios/simulator_host_collector.hpp"

#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>

#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include "core/util/process.hpp"
#include "core/util/time.hpp"

namespace mpi::ios {
namespace {

/// The host's mach timebase, read once.
///
/// On Apple Silicon this is 125/3 -- 41.6667 ns per tick -- and on Intel it
/// is 1/1, which is exactly why reading the counters as nanoseconds went
/// unnoticed for so long anywhere this was tried on an Intel Mac.
const mach_timebase_info_data_t& timebase() {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    if (mach_timebase_info(&t) != KERN_SUCCESS || t.denom == 0) {
      // Identity rather than zero: a wrong-but-plausible number beats a
      // series of zeroes that reads as an idle app.
      t.numer = 1;
      t.denom = 1;
    }
    return t;
  }();
  return tb;
}

constexpr char kProviderCpu[] = "libproc PROC_PIDTASKINFO";
constexpr char kProviderMem[] = "libproc proc_pid_rusage (phys_footprint)";

}  // namespace

std::int64_t mach_ticks_to_ns(std::uint64_t ticks) {
  const mach_timebase_info_data_t& tb = timebase();
  // Done in 128 bits. Splitting the division first -- `(ticks / denom) *
  // numer` -- looks like it avoids overflow and does not: with the Apple
  // Silicon timebase of 125/3, a tick count above about 2^57 still wraps, and
  // a wrapped CPU total reads as the app going backwards in time. A test
  // asserting the conversion stays positive is what found that.
  //
  // 2^64 * 125 needs 71 bits, so unsigned __int128 is exact for every
  // possible input.
  const unsigned __int128 scaled =
      static_cast<unsigned __int128>(ticks) * tb.numer / tb.denom;
  // Saturate rather than wrap. INT64_MAX nanoseconds is 292 years, so this
  // is unreachable for a real process; if it is ever hit, a clamped maximum
  // is at least monotonic, where a wrap is not.
  constexpr unsigned __int128 kMax =
      static_cast<unsigned __int128>(std::numeric_limits<std::int64_t>::max());
  if (scaled > kMax) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(scaled);
}

HostProcessSample sample_host_process(std::int32_t pid) {
  HostProcessSample s;
  if (pid <= 0) {
    s.error = "no pid";
    return s;
  }
  struct proc_taskinfo pti{};
  const int rc = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
  if (rc <= 0) {
    // The app exited, or was never ours to read. Either way this is a lost
    // reading, not a measurement of zero.
    s.error = "PROC_PIDTASKINFO returned nothing for pid " +
              std::to_string(pid) + " (the process may have exited)";
    return s;
  }
  // The unit conversion the whole file exists for. See the header.
  s.cpu_time_ns = mach_ticks_to_ns(
      static_cast<std::uint64_t>(pti.pti_total_user) +
      static_cast<std::uint64_t>(pti.pti_total_system));
  s.resident_bytes = static_cast<std::int64_t>(pti.pti_resident_size);
  s.thread_count = pti.pti_threadnum;

  struct rusage_info_v4 ri{};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V4,
                      reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
    // `phys_footprint` is what Xcode's gauge shows, and unlike the CPU
    // fields it is a byte count with no unit ambiguity.
    s.footprint_bytes = static_cast<std::int64_t>(ri.ri_phys_footprint);
  }
  s.ok = true;
  return s;
}

std::vector<HostThreadSample> sample_host_threads(std::int32_t pid) {
  std::vector<HostThreadSample> out;
  if (pid <= 0) return out;
  // PROC_PIDLISTTHREADS has no size-query form -- asking for one returns
  // ENOMEM -- so a buffer is passed directly.
  constexpr int kMaxThreads = 4096;
  std::vector<std::uint64_t> handles(kMaxThreads);
  const int bytes = proc_pidinfo(
      pid, PROC_PIDLISTTHREADS, 0, handles.data(),
      static_cast<int>(handles.size() * sizeof(std::uint64_t)));
  if (bytes <= 0) return out;
  const int count = bytes / static_cast<int>(sizeof(std::uint64_t));
  for (int i = 0; i < count; i++) {
    struct proc_threadinfo ti{};
    // PROC_PIDTHREADINFO, not PROC_PIDTHREADID64INFO: the latter wants a
    // thread id and these are handles, and it fails with ESRCH on them.
    if (proc_pidinfo(pid, PROC_PIDTHREADINFO, handles[static_cast<std::size_t>(i)],
                     &ti, sizeof(ti)) <= 0) {
      continue;
    }
    HostThreadSample t;
    t.handle = handles[static_cast<std::size_t>(i)];
    // Per-thread counters are already nanoseconds. Converting these through
    // the timebase as well would overstate them by the same factor the task
    // counters were understated by.
    t.cpu_time_ns = static_cast<std::int64_t>(ti.pth_user_time) +
                    static_cast<std::int64_t>(ti.pth_system_time);
    if (ti.pth_name[0] != '\0') {
      t.name.assign(ti.pth_name,
                    strnlen(ti.pth_name, sizeof(ti.pth_name)));
    }
    out.push_back(std::move(t));
  }
  return out;
}

std::optional<bool> is_translated(std::int32_t pid) {
  // `sysctl.proc_translated` answers for the calling process only, so the
  // question is asked of the process itself through kinfo_proc's flags.
  struct kinfo_proc info{};
  std::size_t size = sizeof(info);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) {
    return std::nullopt;
  }
  // P_TRANSLATED is the flag Rosetta sets. Absent on hosts where it is not
  // defined, in which case the question has no answer here rather than a
  // false one.
#ifdef P_TRANSLATED
  return (info.kp_proc.p_flag & P_TRANSLATED) != 0;
#else
  return std::nullopt;
#endif
}

PidLookup find_simulator_app_pid(const std::string& udid,
                                 const std::string& bundle_id,
                                 std::chrono::milliseconds timeout) {
  PidLookup out;
  if (!proc::is_safe_argument(udid, true) ||
      !proc::is_safe_argument(bundle_id, true)) {
    out.error = "refusing to pass an unsafe device id or bundle id";
    return out;
  }
  proc::Options opts;
  opts.timeout = timeout;
  const proc::Result r = proc::run(
      {"xcrun", "simctl", "spawn", udid, "launchctl", "list"}, opts);
  if (!r.spawned) {
    out.error = "could not run simctl: " + r.spawn_error;
    return out;
  }
  if (r.timed_out) {
    out.error = "simctl spawn launchctl did not answer in time";
    return out;
  }
  if (r.exit_code != 0) {
    out.error = "simctl spawn launchctl failed: " + r.err;
    return out;
  }
  // Lines are "<pid>\t<last exit status>\t<label>", and an app's label is
  // `UIKitApplication:<bundle-id>[hex][hex]`.
  const std::string needle = "UIKitApplication:" + bundle_id + "[";
  std::istringstream in(r.out);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(needle) == std::string::npos) continue;
    std::istringstream fields(line);
    std::string pid_field;
    if (!(fields >> pid_field)) continue;
    if (pid_field == "-") {
      // launchd knows the job and it is not running. A real answer, and not
      // the same as the app being absent.
      out.error = "'" + bundle_id + "' is registered with the simulator's "
                  "launchd but is not running";
      return out;
    }
    const long pid = std::strtol(pid_field.c_str(), nullptr, 10);
    if (pid <= 0) continue;
    out.found = true;
    out.pid = static_cast<std::int32_t>(pid);
    return out;
  }
  out.error = "'" + bundle_id + "' is not running in simulator " + udid +
              " (launchctl listed no UIKitApplication job for it)";
  return out;
}

void SimulatorHostCollector::add_point(model::NormalizedTrace& out,
                                       const std::string& name,
                                       const std::string& family,
                                       const std::string& unit,
                                       model::TimeNs at_ns, double value) {
  for (auto& c : out.counters) {
    if (c.name == name) {
      c.points.emplace_back(at_ns, value);
      return;
    }
  }
  model::CounterSeries c;
  c.name = name;
  c.unit = unit;
  c.provider = family == "cpu_time" || family == "cpu_utilisation"
                   ? kProviderCpu
                   : kProviderMem;
  c.process_instance_id = process_key_;
  c.family = family;
  c.points.emplace_back(at_ns, value);
  out.counters.push_back(std::move(c));
}

session::CaptureResult SimulatorHostCollector::begin(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  (void)config;
  (void)out;
  session::CaptureResult result;
  if (device.form != model::DeviceForm::kSimulator) {
    // The whole approach rests on the app being a host process. On a
    // physical device it is not, and there is no fallback to degrade to.
    result.error =
        "this collector works for iOS simulators only: it reads the app as a "
        "host process, which is what a simulator gives and a physical device "
        "does not. For a device, `record` uses xctrace, which cannot stream.";
    return result;
  }
  if (processes.empty()) {
    result.error = "no target process";
    return result;
  }
  process_key_ = processes.front().canonical();
  bundle_id_ = processes.front().app.app_identifier;

  const PidLookup pid = find_simulator_app_pid(
      device.capture_id(), bundle_id_, std::chrono::milliseconds(10000));
  if (!pid.found) {
    result.error = pid.error;
    return result;
  }
  pid_ = pid.pid;
  translated_ = is_translated(pid_);
  started_ = true;

  const HostProcessSample first = sample_host_process(pid_);
  if (!first.ok) {
    result.error = "found pid " + std::to_string(pid_) +
                   " but could not read it: " + first.error;
    started_ = false;
    return result;
  }

  result.started = true;
  result.any_data = true;

  // One capability per source, which is where this model keeps "what ran,
  // what it covers, and what it does not".
  const std::string observed = time_util::now_iso8601_utc();
  auto source = [&](const std::string& cap_id, const std::string& name,
                    model::CapabilityStatus status, const std::string& evidence,
                    std::vector<std::string> limitations) {
    model::Capability c;
    c.id = cap_id;
    c.human_name = name;
    c.status = status;
    c.provider = "libproc";
    c.tested = model::TestedState::kVerifiedOnSimulatorOrEmulator;
    c.observed_at = observed;
    c.evidence = evidence;
    c.limitations = std::move(limitations);
    result.source_results.push_back(std::move(c));
  };

  source("ios.simulator.host_cpu_time", "CPU time (host process)",
         model::CapabilityStatus::kLimited,
         "PROC_PIDTASKINFO for host process " + std::to_string(pid_) +
             ", converted from mach absolute time units to nanoseconds",
         {"this is CPU *time*, not attribution: stack sampling needs "
          "task_for_pid, which is refused without root, so no function-level "
          "answer is available here"});

  source("ios.simulator.host_footprint", "Memory footprint",
         model::CapabilityStatus::kAvailable,
         "proc_pid_rusage ri_phys_footprint, the value Xcode's memory gauge "
         "shows",
         {});

  source("ios.simulator.frames", "Frame timing",
         // Tested and genuinely unsupported. A different answer from "not
         // tested", and the whole reason this is recorded rather than left
         // as an empty series.
         model::CapabilityStatus::kUnsupported,
         "no command-line frame-timing source exists for an iOS simulator",
         {"no frames were measured, which is not the same as zero frames: "
          "nothing in this capture can support a frame-deadline finding"});

  if (translated_.has_value() && *translated_) {
    source("ios.simulator.rosetta_translated", "Rosetta translation",
           model::CapabilityStatus::kLimited,
           "kinfo_proc reports P_TRANSLATED for this process",
           {"the app runs TRANSLATED under Rosetta, so its CPU numbers are "
            "timings of translated code -- comparable neither to a native "
            "simulator build nor to a device"});
  }
  return result;
}

session::LiveUpdate SimulatorHostCollector::tick(
    const model::DeviceRef& /*device*/,
    const std::vector<model::ProcessInstance>& /*processes*/,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::LiveUpdate update;
  const auto tick_started = std::chrono::steady_clock::now();
  const model::TimeNs at_ns = time_util::monotonic_ns();
  update.at_ns = at_ns;
  if (!started_) {
    update.notes.push_back("the capture was never started");
    return update;
  }
  ticks_++;

  const HostProcessSample s = sample_host_process(pid_);
  if (!s.ok) {
    dead_ticks_++;
    // Three in a row is the app having gone, not a transient. Matching the
    // Android loop's rule so a lost app is reported the same way on both.
    if (dead_ticks_ >= 3) {
      update.notes.push_back(
          "the process stopped answering for three consecutive ticks: it has "
          "exited or been terminated. What was collected before that point "
          "is real; the absence after it is a lost target, not a quiet app.");
    }
    update.tick_cost = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tick_started);
    return update;
  }
  dead_ticks_ = 0;

  std::int64_t added = 0;
  if (config.memory) {
    add_point(out, "memory.footprint_bytes", "footprint", "bytes", at_ns,
              static_cast<double>(s.footprint_bytes));
    add_point(out, "memory.rss_bytes", "rss", "bytes", at_ns,
              static_cast<double>(s.resident_bytes));
    added += 2;
  }
  if (config.cpu_samples) {
    add_point(out, "cpu.process_time_ns", "cpu_time", "ns", at_ns,
              static_cast<double>(s.cpu_time_ns));
    added++;
    // A rate needs two readings. The first tick publishes none rather than
    // 0%, which would read as an idle app at the moment of launch -- the one
    // moment it is certainly not idle.
    if (last_cpu_ns_.has_value() && last_at_ns_.has_value() &&
        at_ns > *last_at_ns_) {
      const double wall = static_cast<double>(at_ns - *last_at_ns_);
      const double cpu = static_cast<double>(s.cpu_time_ns - *last_cpu_ns_);
      if (cpu >= 0.0) {
        add_point(out, "cpu.utilisation_percent", "cpu_utilisation", "percent",
                  at_ns, cpu / wall * 100.0);
        added++;
      }
    }
    last_cpu_ns_ = s.cpu_time_ns;
    last_at_ns_ = at_ns;

    // Per-thread times, which is what gives iOS the JS-versus-native split.
    for (const HostThreadSample& t : sample_host_threads(pid_)) {
      if (t.name.empty()) continue;   // unnamed: not guessed at
      const auto it = known_threads_.find(t.handle);
      if (it == known_threads_.end()) {
        known_threads_.emplace(t.handle, t.name);
        model::ThreadInfo info;
        info.thread_instance_id = process_key_ + ":t" + std::to_string(t.handle);
        info.process_instance_id = process_key_;
        // A mach thread handle is not a tid, and claiming one would be a
        // false identifier. Left at 0 rather than filled with a lookalike.
        info.tid = 0;
        info.name = t.name;
        // The runtime names its own threads, so this is a platform signal
        // rather than our pattern-matching: React Native's JS thread is
        // `com.facebook.react.runtime.JavaScript`.
        info.is_js_thread =
            t.name.find("react.runtime.JavaScript") != std::string::npos ||
            t.name.find("hermes") != std::string::npos;
        out.threads.push_back(std::move(info));
      }
      add_point(out, "cpu.thread_time_ns." + t.name, "cpu_time", "ns", at_ns,
                static_cast<double>(t.cpu_time_ns));
      added++;
    }
  }
  update.new_counter_points = added;
  // Frames are not reported as zero. The Live UI shows the source status, and
  // this is the source that has none.
  if (config.frames && ticks_ == 1) {
    update.notes.push_back(
        "frames: no provider on a simulator. Not zero frames -- no frame "
        "source exists here, so nothing in this capture can support a "
        "frame-deadline finding.");
  }
  update.tick_cost = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - tick_started);
  return update;
}

session::CaptureResult SimulatorHostCollector::finish(
    const model::DeviceRef& /*device*/,
    const std::vector<model::ProcessInstance>& /*processes*/,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::CaptureResult result;
  result.started = started_;
  result.any_data = started_ && ticks_ > 0;
  if (!started_) {
    result.error = "the capture was never started";
    return result;
  }
  if (config.frames) {
    // A coverage row whose whole window is one gap, so a detector cannot read
    // the silence as measured smoothness. Spec section 6: a window a
    // collector did not cover is not a window in which it measured zero.
    model::Coverage cov;
    cov.collector = "frames (none on a simulator)";
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = 0;
    model::CoverageGap gap;
    gap.collector = cov.collector;
    gap.start_ns = out.window_start_ns;
    gap.end_ns = out.window_end_ns;
    gap.reason = "no_frame_source_for_ios_simulator";
    cov.gaps.push_back(std::move(gap));
    out.coverage.push_back(std::move(cov));
  }
  return result;
}

session::CaptureResult SimulatorHostCollector::capture(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  // A batch capture here would be a streaming capture with the ticks hidden.
  // `record` on a simulator already goes through xctrace, which gives frames
  // and stacks this cannot; degrading that silently would be a downgrade
  // disguised as a success.
  (void)device;
  (void)processes;
  (void)config;
  (void)out;
  session::CaptureResult r;
  r.error = "this collector is for live capture only. For a batch recording "
            "on a simulator, xctrace provides frames and stacks that reading "
            "the host process cannot.";
  return r;
}

}  // namespace mpi::ios
