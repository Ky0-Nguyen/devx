#include "adapters/android/adb_collector.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>
#include <thread>

#include "core/util/process.hpp"
#include "core/util/time.hpp"

namespace mpi::android {
namespace {

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream ss(s);
  while (std::getline(ss, cur, sep)) out.push_back(cur);
  return out;
}

bool all_digits(const std::string& s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

std::int64_t to_i64(const std::string& s, std::int64_t fallback = 0) {
  const std::string t = trim(s);
  if (t.empty()) return fallback;
  const bool negative = t[0] == '-';
  const std::string digits = negative ? t.substr(1) : t;
  if (!all_digits(digits)) return fallback;
  errno = 0;
  const long long v = std::strtoll(t.c_str(), nullptr, 10);
  return errno == 0 ? static_cast<std::int64_t>(v) : fallback;
}

model::Capability make_source(const char* id, const char* human,
                              model::CapabilityStatus status,
                              const char* provider) {
  model::Capability c;
  c.id = id;
  c.human_name = human;
  c.status = status;
  c.provider = provider;
  c.observed_at = time_util::now_iso8601_utc();
  return c;
}

}  // namespace

FrameStats parse_gfxinfo_framestats(const std::string& text) {
  FrameStats out;
  const auto lines = split(text, '\n');

  bool in_block = false;
  std::map<std::string, std::size_t> col;
  std::size_t expected_columns = 0;

  for (const auto& raw : lines) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim(line);

    if (t == "---PROFILEDATA---") {
      // Each block is preceded by its own header; entering a block resets the
      // column map so a second block with a different schema is still read
      // correctly rather than silently misaligned.
      in_block = !in_block;
      if (in_block) {
        col.clear();
        expected_columns = 0;
      }
      continue;
    }
    if (!in_block || t.empty()) continue;

    if (t.rfind("Flags,", 0) == 0) {
      const auto headers = split(t, ',');
      for (std::size_t i = 0; i < headers.size(); ++i) {
        const std::string name = trim(headers[i]);
        if (!name.empty()) col[name] = i;
      }
      expected_columns = headers.size();
      // These four are the minimum DET-01 can work from.
      for (const char* required : {"IntendedVsync", "FrameDeadline", "FrameCompleted"}) {
        if (col.find(required) == col.end()) {
          out.warnings.push_back(
              std::string("framestats header lacks '") + required +
              "'; frame records cannot be built from this block");
          col.clear();
          break;
        }
      }
      continue;
    }

    if (col.empty()) continue;  // header was unusable; do not guess
    const auto fields = split(t, ',');
    if (expected_columns > 0 && fields.size() + 1 < expected_columns) {
      // A short row is skipped rather than padded with zeros, which would look
      // like a frame that started at time 0.
      continue;
    }
    auto field = [&](const char* name) -> std::int64_t {
      auto it = col.find(name);
      if (it == col.end() || it->second >= fields.size()) return 0;
      return to_i64(fields[it->second]);
    };

    FrameStatsRow r;
    r.flags = field("Flags");
    r.intended_vsync = field("IntendedVsync");
    r.vsync = field("Vsync");
    r.frame_deadline = field("FrameDeadline");
    r.frame_completed = field("FrameCompleted");
    r.gpu_completed = field("GpuCompleted");
    r.display_present_time = field("DisplayPresentTime");
    r.frame_interval = field("FrameInterval");
    r.draw_start = field("DrawStart");

    // A non-zero Flags value marks a frame the platform says should be
    // excluded from jank statistics (first draw, window layout change).
    if (r.flags != 0) continue;
    if (r.intended_vsync <= 0 || r.frame_completed <= 0) continue;
    if (r.frame_interval > 0 &&
        std::find(out.observed_frame_intervals.begin(),
                  out.observed_frame_intervals.end(),
                  r.frame_interval) == out.observed_frame_intervals.end()) {
      out.observed_frame_intervals.push_back(r.frame_interval);
    }
    out.rows.push_back(r);
  }
  return out;
}

SimpleperfSamples parse_simpleperf_report_sample(const std::string& text) {
  SimpleperfSamples out;
  const auto lines = split(text, '\n');

  enum class Section { kNone, kMeta, kSample, kCallchain };
  Section section = Section::kNone;
  SimpleperfSamples::Sample current;
  bool have_current = false;
  // simpleperf lists the leaf symbol first and then the callchain from the
  // leaf's caller outwards, so frames are collected then reversed.
  std::vector<std::string> leaf_first;

  auto flush = [&]() {
    if (!have_current) return;
    leaf_first.erase(std::remove(leaf_first.begin(), leaf_first.end(), std::string()),
                     leaf_first.end());
    current.frames.assign(leaf_first.rbegin(), leaf_first.rend());
    if (!current.frames.empty()) out.samples.push_back(current);
    current = SimpleperfSamples::Sample{};
    leaf_first.clear();
    have_current = false;
  };

  for (const auto& raw : lines) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim(line);
    if (t.empty()) continue;

    if (t == "meta_info:") {
      flush();
      section = Section::kMeta;
      continue;
    }
    if (t == "sample:") {
      flush();
      section = Section::kSample;
      have_current = true;
      continue;
    }
    if (t == "callchain:") {
      section = Section::kCallchain;
      continue;
    }

    const std::size_t colon = t.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(t.substr(0, colon));
    const std::string value = trim(t.substr(colon + 1));

    if (section == Section::kMeta) {
      if (key == "event_type") out.event_type = value;
      else if (key == "app_package_name") out.app_package_name = value;
      else if (key == "app_type") out.app_type = value;
      else if (key == "android_build_type") out.build_type = value;
      else if (key == "android_sdk_version" && all_digits(value)) {
        out.sdk_version = std::atoi(value.c_str());
      }
      continue;
    }

    if (!have_current) continue;

    if (key == "symbol") {
      // Both the leaf's own symbol and each callchain entry land here; order
      // of arrival is leaf first, which the reversal above relies on.
      leaf_first.push_back(value);
    } else if (section == Section::kSample) {
      if (key == "time") current.time_ns = to_i64(value);
      else if (key == "event_count") current.event_count = to_i64(value);
      else if (key == "thread_id") {
        current.thread_id = static_cast<std::int32_t>(to_i64(value));
      } else if (key == "thread_name") current.thread_name = value;
    }
  }
  flush();

  if (out.samples.empty() && !text.empty()) {
    out.warnings.push_back(
        "simpleperf produced output but no sample carried a symbolised frame");
  }
  return out;
}

MemInfo parse_dumpsys_meminfo(const std::string& text) {
  MemInfo out;
  const auto lines = split(text, '\n');
  // Kilobytes in the report; the model stores bytes.
  const double kKiB = 1024.0;

  for (const auto& raw : lines) {
    const std::string t = trim(raw);
    if (t.empty()) continue;

    if (t.rfind("** MEMINFO in pid ", 0) == 0) {
      const std::string rest = t.substr(18);
      const std::size_t sp = rest.find(' ');
      const std::string pid_s = sp == std::string::npos ? rest : rest.substr(0, sp);
      if (all_digits(pid_s)) {
        out.pid = static_cast<std::int32_t>(to_i64(pid_s));
      }
      continue;
    }

    // Rows are "Label  Pss Private Private SwapPss Rss ..." with the label
    // possibly containing spaces, so the numbers are taken from the tail.
    auto tail_numbers = [&](const std::string& label) -> std::vector<double> {
      if (t.rfind(label, 0) != 0) return {};
      std::istringstream ss(t.substr(label.size()));
      std::vector<double> nums;
      std::string tok;
      while (ss >> tok) {
        if (!all_digits(tok)) break;
        nums.push_back(static_cast<double>(to_i64(tok)) * kKiB);
      }
      return nums;
    };

    if (auto n = tail_numbers("Native Heap"); n.size() >= 5) {
      out.native_heap_rss_bytes = n[4];
    } else if (auto d = tail_numbers("Dalvik Heap"); d.size() >= 5) {
      out.dalvik_heap_rss_bytes = d[4];
    } else if (auto tot = tail_numbers("TOTAL"); tot.size() >= 5) {
      // The TOTAL row's columns match the header: Pss, PrivateDirty,
      // PrivateClean, SwapPss, Rss.
      out.pss_total_bytes = tot[0];
      out.private_dirty_bytes = tot[1];
      out.rss_total_bytes = tot[4];
    }
  }
  return out;
}

AdbCollector::AdbCollector(std::string adb_path) : adb_path_(std::move(adb_path)) {}

std::vector<std::string> AdbCollector::shell_argv(
    const std::string& serial, const std::vector<std::string>& args) const {
  std::vector<std::string> argv{adb_path_};
  if (!serial.empty()) {
    argv.push_back("-s");
    argv.push_back(serial);
  }
  argv.push_back("shell");
  for (const auto& a : args) argv.push_back(a);
  return argv;
}

session::CaptureResult AdbCollector::capture(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::CaptureResult result;
  const auto started_at = std::chrono::steady_clock::now();

  if (processes.empty()) {
    result.error = "no process instance was supplied; nothing to capture";
    return result;
  }
  const std::string package = processes.front().app.app_identifier;
  if (!proc::is_safe_argument(package, /*reject_option_like=*/true)) {
    result.error = "refusing to capture identifier '" + package +
                   "': it would be read as a command-line option";
    return result;
  }
  // The primary process is the capture subject; secondary processes are
  // recorded on the trace but not separately sampled in this version.
  const model::ProcessInstance* primary = &processes.front();
  for (const auto& p : processes) {
    if (p.is_primary) primary = &p;
  }
  const std::string process_key = primary->canonical();

  proc::Options po;
  po.timeout = config.duration + std::chrono::milliseconds(30000);
  po.cancel = config.cancel;

  result.started = true;
  out.device = device;

  // The device's boot-relative clock is what every source below timestamps
  // against, so it is declared once and used as the primary domain.
  const std::string clock_id = "android.boottime.ns";
  if (out.primary_clock_domain.empty()) out.primary_clock_domain = clock_id;
  model::ClockDomain cd;
  cd.id = clock_id;
  cd.base = "monotonic";
  cd.provider = "android.kernel.boottime";
  cd.monotonic = true;
  out.clock_domains.push_back(cd);

  // ---- reset the frame history so only this capture's frames are read ------
  bool frames_reset = false;
  if (config.frames && config.reset_frame_history) {
    const auto r = proc::run(
        shell_argv(device.device_id, {"dumpsys", "gfxinfo", package, "reset"}), po);
    frames_reset = r.ok();
    if (!frames_reset) {
      result.source_results.push_back([&] {
        auto c = make_source("android.capture.frames.reset",
                             "Frame history reset before capture",
                             model::CapabilityStatus::kLimited, "dumpsys gfxinfo");
        c.evidence = "`dumpsys gfxinfo " + package + " reset` failed: " +
                     (r.spawned ? trim(r.err) : r.spawn_error);
        c.limitations.push_back(
            "frames from before this capture may be included, so the window is "
            "wider than the capture itself");
        c.tested = model::TestedState::kProbedOnly;
        return c;
      }());
    }
  }

  // ---- CPU sampling, which is the long-running source ---------------------
  // simpleperf is started first and runs for the configured duration; the
  // memory poller runs alongside it in this thread.
  const std::string perf_path = "/data/local/tmp/mpi-perf.data";
  bool cpu_started = false;
  std::string cpu_error;
  if (config.cpu_samples) {
    std::vector<std::string> args{
        "simpleperf", "record", "--app", package, "-o", perf_path,
        "--duration", std::to_string(config.duration.count() / 1000),
        "-f", std::to_string(config.sample_frequency_hz), "-g", "-e", "cpu-clock"};
    // Run synchronously. Backgrounding it would need a shell, and argv has no
    // way to express "&" -- and a synchronous child is reaped deterministically
    // when the capture is cancelled, which matters more than overlapping the
    // memory read with it.
    const auto r = proc::run(shell_argv(device.device_id, args), po);
    cpu_started = r.spawned;
    if (!r.ok()) {
      cpu_error = r.spawned ? trim(r.err) : r.spawn_error;
    }
  }

  // The requested duration is the *capture window*, not just the CPU
  // recording. If simpleperf fails immediately -- as it does on a target that
  // is not debuggable -- the remaining sources would otherwise observe a few
  // hundred milliseconds and report nothing, which looks like "the app did
  // nothing" rather than "the window was too short". So the window is held
  // open for the time that was asked for.
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    auto remaining = config.duration - elapsed;
    while (remaining.count() > 0 && !config.cancel.cancelled()) {
      const auto slice = std::min(remaining, std::chrono::milliseconds(100));
      std::this_thread::sleep_for(slice);
      remaining -= slice;
    }
  }

  // ---- memory counters ----------------------------------------------------
  // Sampled after the CPU recording so the two describe the same interval;
  // a single reading is honest about being a single reading.
  if (config.memory) {
    const auto r = proc::run(
        shell_argv(device.device_id, {"dumpsys", "meminfo", package}), po);
    auto c = make_source("android.capture.memory", "Process memory counters",
                         model::CapabilityStatus::kUnknown, "dumpsys meminfo");
    if (r.ok()) {
      const MemInfo mem = parse_dumpsys_meminfo(r.out);
      const model::TimeNs now = 0;  // placed at the window start below
      auto add = [&](const char* name, const char* family,
                     const std::optional<double>& v) {
        if (!v.has_value()) return;
        model::CounterSeries s;
        s.name = name;
        s.unit = "bytes";
        s.provider = "dumpsys meminfo";
        s.process_instance_id = process_key;
        s.family = family;
        s.points.emplace_back(now, *v);
        out.counters.push_back(std::move(s));
      };
      // Each family stays its own series; spec section 8 forbids summing or
      // equating them.
      add("memory.rss_total_bytes", "rss", mem.rss_total_bytes);
      add("memory.pss_total_bytes", "pss", mem.pss_total_bytes);
      add("memory.private_dirty_bytes", "private_dirty", mem.private_dirty_bytes);
      add("memory.native_heap_rss_bytes", "native_heap", mem.native_heap_rss_bytes);
      add("memory.dalvik_heap_rss_bytes", "dalvik_heap", mem.dalvik_heap_rss_bytes);

      c.status = out.counters.empty() ? model::CapabilityStatus::kLimited
                                      : model::CapabilityStatus::kAvailable;
      c.evidence = "`dumpsys meminfo " + package + "` yielded " +
                   std::to_string(out.counters.size()) + " counter series";
      c.limitations.push_back(
          "a single instantaneous reading, not a time series: growth cannot be "
          "assessed from one point");
      c.limitations.push_back(
          "RSS, PSS, private-dirty and the heaps are separate families and are "
          "never summed");
      c.tested = device.form == model::DeviceForm::kPhysical
                     ? model::TestedState::kVerifiedOnPhysicalDevice
                     : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    } else {
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "`dumpsys meminfo` failed: " +
                   (r.spawned ? trim(r.err) : r.spawn_error);
      c.tested = model::TestedState::kProbedOnly;
    }
    result.source_results.push_back(std::move(c));
  }

  // ---- read back the CPU samples -----------------------------------------
  if (config.cpu_samples) {
    auto c = make_source("android.capture.cpu_samples", "Sampled CPU stacks",
                         model::CapabilityStatus::kUnknown, "simpleperf");
    c.prerequisites.push_back(
        "the target package is debuggable or declares <profileable "
        "android:shell=\"true\"/>; simpleperf --app routes through run-as");
    if (!cpu_started) {
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "simpleperf could not be started: " + cpu_error;
      c.tested = model::TestedState::kProbedOnly;
    } else {
      const auto rep = proc::run(
          shell_argv(device.device_id,
                     {"simpleperf", "report-sample", "--show-callchain", "-i",
                      perf_path}),
          po);
      if (!rep.ok()) {
        // A permission failure is the expected outcome on a non-profileable
        // target, and is reported as such rather than as an empty result.
        // The recording step's own error is the informative one; the report
        // step can only ever say the file is missing.
        const std::string report_detail =
            rep.spawned ? trim(rep.err) : rep.spawn_error;
        const std::string& detail = cpu_error.empty() ? report_detail : cpu_error;
        const bool denied =
            detail.find("Permission denied") != std::string::npos ||
            detail.find("isn't debuggable") != std::string::npos ||
            detail.find("not debuggable") != std::string::npos ||
            detail.find("debuggable/profileable") != std::string::npos;
        c.status = denied ? model::CapabilityStatus::kPermissionDenied
                          : model::CapabilityStatus::kUnsupported;
        c.evidence = "simpleperf could not record: " + detail;
        c.recovery_action =
            denied
                ? "this target is not debuggable or profileable, which is "
                  "Android's own restriction and is not bypassed here. Install "
                  "a debuggable build, or add <profileable "
                  "android:shell=\"true\"/> to the target's manifest."
                : "check that simpleperf is present and the device is authorized";
        c.tested = model::TestedState::kProbedOnly;
      } else {
        const SimpleperfSamples parsed = parse_simpleperf_report_sample(rep.out);
        std::map<std::int32_t, std::string> thread_keys;
        for (const auto& s : parsed.samples) {
          const std::string tkey =
              process_key + "|tid=" + std::to_string(s.thread_id);
          if (thread_keys.find(s.thread_id) == thread_keys.end()) {
            thread_keys[s.thread_id] = tkey;
            model::ThreadInfo ti;
            ti.thread_instance_id = tkey;
            ti.process_instance_id = process_key;
            ti.tid = s.thread_id;
            ti.name = s.thread_name;
            // The main thread carries the process name on Android.
            ti.is_main_ui_thread = s.thread_name == package ||
                                   s.thread_name == "main";
            // Thread names observed on a real React Native 0.7x debug build:
          // the JS thread is "mqt_v_js" here, and older builds use "mqt_js".
          ti.is_js_thread = s.thread_name == "mqt_js" ||
                            s.thread_name == "mqt_v_js" ||
                            s.thread_name.rfind("mqt_js", 0) == 0 ||
                            s.thread_name.find("hermes") != std::string::npos ||
                            s.thread_name.find("Hermes") != std::string::npos;
            out.threads.push_back(std::move(ti));
          }
          model::CpuSample smp;
          smp.timestamp_ns = s.time_ns;
          smp.process_instance_id = process_key;
          smp.thread_instance_id = tkey;
          smp.provider = "simpleperf";
          smp.weight = s.event_count > 0 ? std::optional<double>(1.0) : std::nullopt;
          smp.frames = s.frames;
          out.cpu_samples.push_back(std::move(smp));

          model::Event ev;
          ev.event_id = "simpleperf-" + std::to_string(out.events.size());
          ev.provider = "simpleperf";
          ev.clock_domain = clock_id;
          ev.timestamp = s.time_ns;
          ev.process_instance_id = process_key;
          ev.thread_instance_id = tkey;
          ev.category = model::EventCategory::kCpuSample;
          ev.name = out.cpu_samples.back().frames.empty()
                        ? std::string("(unsymbolised)")
                        : out.cpu_samples.back().frames.back();
          out.events.push_back(std::move(ev));
        }

        // simpleperf's meta_info carries build facts read off the device, which
        // are stronger than anything inferred host-side.
        if (!parsed.app_type.empty()) {
          model::BuildFact f;
          f.key = "native.debuggable";
          f.value = parsed.app_type;
          f.boolean_value = parsed.app_type == "debuggable" ? model::Tri::kTrue
                                                            : model::Tri::kFalse;
          f.source = model::FactSource::kDeviceProvider;
          f.observed_at = time_util::now_iso8601_utc();
          f.basis = "simpleperf meta_info app_type=" + parsed.app_type;
          out.build.upsert(std::move(f));
        }
        if (!parsed.build_type.empty()) {
          model::BuildFact f;
          f.key = "device.build_type";
          f.value = parsed.build_type;
          f.source = model::FactSource::kDeviceProvider;
          f.observed_at = time_util::now_iso8601_utc();
          f.basis = "simpleperf meta_info android_build_type";
          out.build.upsert(std::move(f));
        }
        if (parsed.sdk_version.has_value()) {
          model::BuildFact f;
          f.key = "device.sdk_version";
          f.value = std::to_string(*parsed.sdk_version);
          f.source = model::FactSource::kDeviceProvider;
          f.observed_at = time_util::now_iso8601_utc();
          f.basis = "simpleperf meta_info android_sdk_version";
          out.build.upsert(std::move(f));
        }

        c.status = parsed.samples.empty() ? model::CapabilityStatus::kLimited
                                          : model::CapabilityStatus::kAvailable;
        c.evidence = "simpleperf recorded " +
                     std::to_string(parsed.samples.size()) +
                     " symbolised sample(s) at " +
                     std::to_string(config.sample_frequency_hz) + " Hz for " +
                     std::to_string(config.duration.count()) + " ms";
        c.scope = "user-mode stacks of the target app only (cpu-clock:u)";
        c.limitations.push_back(
            "kernel stacks are not recorded, so time spent in the kernel is "
            "attributed to its user-mode caller");
        if (parsed.samples.empty()) {
          c.limitations.push_back(
              "no sample carried a symbol; the app may have been idle for the "
              "whole window");
        }
        for (const auto& w : parsed.warnings) c.limitations.push_back(w);
        c.tested = device.form == model::DeviceForm::kPhysical
                       ? model::TestedState::kVerifiedOnPhysicalDevice
                       : model::TestedState::kVerifiedOnSimulatorOrEmulator;
        if (!parsed.samples.empty()) result.any_data = true;
      }
    }
    result.source_results.push_back(std::move(c));

    // Remove only the file this session created (spec D22).
    proc::run(shell_argv(device.device_id, {"rm", "-f", perf_path}), po);
  }

  // ---- frames -------------------------------------------------------------
  if (config.frames) {
    auto c = make_source("android.capture.frames", "Frame timing",
                         model::CapabilityStatus::kUnknown, "dumpsys gfxinfo");
    const auto r = proc::run(
        shell_argv(device.device_id,
                   {"dumpsys", "gfxinfo", package, "framestats"}),
        po);
    if (!r.ok()) {
      c.status = model::CapabilityStatus::kUnsupported;
      c.evidence = "`dumpsys gfxinfo " + package + " framestats` failed: " +
                   (r.spawned ? trim(r.err) : r.spawn_error);
      c.tested = model::TestedState::kProbedOnly;
    } else {
      const FrameStats fs = parse_gfxinfo_framestats(r.out);
      for (std::size_t i = 0; i < fs.rows.size(); ++i) {
        const auto& row = fs.rows[i];
        model::FrameRecord fr;
        fr.event_id = "frame-" + std::to_string(i);
        fr.start_ns = row.intended_vsync;
        // FrameDeadline is an absolute instant; the model wants a duration.
        if (row.frame_deadline > row.intended_vsync) {
          fr.deadline_ns = row.frame_deadline - row.intended_vsync;
        }
        // DisplayPresentTime is the real presentation instant but is zero when
        // the platform does not supply it, as on an emulator. FrameCompleted
        // is then the best available and the source is labelled accordingly --
        // never as presentation truth.
        if (row.display_present_time > row.intended_vsync) {
          fr.presented_ns = row.display_present_time;
          fr.source = model::FrameSource::kPresentationTimestamps;
        } else {
          fr.presented_ns = row.frame_completed;
          fr.source = model::FrameSource::kFrameDeadlineReports;
        }
        if (row.gpu_completed > row.draw_start && row.draw_start > 0) {
          fr.cpu_duration_ns = row.gpu_completed - row.draw_start;
        }
        fr.process_instance_id = process_key;
        fr.surface = package;
        out.frames.push_back(std::move(fr));
      }

      // The refresh rate comes from the platform's own FrameInterval rather
      // than being assumed. More than one distinct value means the rate
      // changed during the capture, which makes a single deadline indefensible.
      if (!fs.observed_frame_intervals.empty()) {
        model::TimeNs first = out.frames.empty() ? 0 : out.frames.front().start_ns;
        model::TimeNs last = out.frames.empty()
                                 ? 0
                                 : out.frames.back().presented_ns.value_or(
                                       out.frames.back().start_ns);
        model::RefreshInterval ri;
        ri.start_ns = first;
        ri.end_ns = last;
        ri.provider = "dumpsys gfxinfo FrameInterval";
        if (fs.observed_frame_intervals.size() == 1 &&
            fs.observed_frame_intervals.front() > 0) {
          ri.hz = 1e9 / static_cast<double>(fs.observed_frame_intervals.front());
          ri.variable = false;
        } else {
          ri.variable = true;
          c.limitations.push_back(
              "the platform reported " +
              std::to_string(fs.observed_frame_intervals.size()) +
              " distinct frame intervals during the capture, so the refresh "
              "rate is variable and no single deadline is derivable");
        }
        out.refresh_intervals.push_back(std::move(ri));
      }

      c.status = fs.rows.empty() ? model::CapabilityStatus::kLimited
                                 : model::CapabilityStatus::kAvailable;
      c.evidence = "framestats yielded " + std::to_string(fs.rows.size()) +
                   " frame record(s)";
      bool any_presentation = false;
      for (const auto& f : out.frames) {
        if (f.source == model::FrameSource::kPresentationTimestamps) {
          any_presentation = true;
        }
      }
      if (!any_presentation && !out.frames.empty()) {
        c.limitations.push_back(
            "DisplayPresentTime was not supplied, so frame completion is used "
            "instead of presentation; findings from it stay qualified");
      }
      if (!frames_reset && config.reset_frame_history) {
        c.limitations.push_back(
            "the frame history could not be reset before the capture");
      }
      if (!config.reset_frame_history) {
        c.limitations.push_back(
            "frame history was NOT reset, so these frames may predate the "
            "capture window and the window was widened to contain them");
      }
      for (const auto& w : fs.warnings) c.limitations.push_back(w);
      c.tested = device.form == model::DeviceForm::kPhysical
                     ? model::TestedState::kVerifiedOnPhysicalDevice
                     : model::TestedState::kVerifiedOnSimulatorOrEmulator;
      if (!fs.rows.empty()) result.any_data = true;
    }
    result.source_results.push_back(std::move(c));
  }

  // ---- window and coverage -----------------------------------------------
  // Derived from what was actually collected, so an empty source produces a
  // gap rather than a zero-length window that hides it.
  model::TimeNs lo = 0;
  model::TimeNs hi = 0;
  bool have_window = false;
  auto extend = [&](model::TimeNs a, model::TimeNs b) {
    if (a <= 0) return;
    if (!have_window) {
      lo = a;
      hi = b;
      have_window = true;
    } else {
      lo = std::min(lo, a);
      hi = std::max(hi, b);
    }
  };
  for (const auto& f : out.frames) extend(f.start_ns, f.presented_ns.value_or(f.start_ns));
  for (const auto& s : out.cpu_samples) extend(s.timestamp_ns, s.timestamp_ns);
  if (have_window) {
    out.window_start_ns = lo;
    out.window_end_ns = hi;
  }
  // The memory reading was taken at the end of the capture, so its point is
  // placed there rather than at zero.
  for (auto& s : out.counters) {
    for (auto& pt : s.points) pt.first = out.window_end_ns;
  }

  for (const auto& c : result.source_results) {
    if (c.status == model::CapabilityStatus::kAvailable ||
        c.status == model::CapabilityStatus::kLimited) {
      continue;
    }
    // A source that did not run covers none of the window.
    model::Coverage cov;
    cov.collector = c.provider;
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = 0;
    model::CoverageGap gap;
    gap.collector = c.provider;
    gap.start_ns = out.window_start_ns;
    gap.end_ns = out.window_end_ns;
    gap.reason = std::string("source_") + model::to_string(c.status);
    cov.gaps.push_back(std::move(gap));
    out.coverage.push_back(std::move(cov));
  }

  if (config.cancel.cancelled()) {
    out.partial = true;
    out.partial_reasons.push_back("capture cancelled by the operator");
  }
  if (!result.any_data) {
    out.partial = true;
    out.partial_reasons.push_back("no source produced data");
    if (result.error.empty()) {
      result.error = "capture completed but no source produced data; see the "
                     "per-source results";
    }
  }

  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
  return result;
}

}  // namespace mpi::android
