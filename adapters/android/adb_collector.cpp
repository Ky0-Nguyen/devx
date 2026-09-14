#include "adapters/android/adb_collector.hpp"

#include "adapters/android/atrace_parser.hpp"

#include <algorithm>
#include <cmath>
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

// Writes the coverage row for the two tick-collected sources.
//
// Shared by the streaming and the batch path, and that sharing is the point.
// The streaming path grew these rows because "a source with no row is
// indistinguishable from a source that measured nothing" -- H11 -- while the
// batch path wrote rows only for sources that *failed*. So a batch capture
// where frames ran and reported nothing came out looking exactly like one
// where frames never ran, which is the confusion the rows exist to prevent.
// One function now, called from both, so the next path added cannot quietly
// skip it.
void record_tick_source_coverage(model::NormalizedTrace& out,
                                 const session::CaptureConfig& config,
                                 const std::vector<model::CoverageGap>& frame_gaps,
                                 bool check_cadence) {
  if (config.frames) {
    model::Coverage cov;
    cov.collector = "dumpsys gfxinfo";
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = static_cast<std::int64_t>(out.frames.size());
    cov.gaps = frame_gaps;
    if (out.frames.empty()) {
      model::CoverageGap gap;
      gap.collector = "dumpsys gfxinfo";
      gap.start_ns = out.window_start_ns;
      gap.end_ns = out.window_end_ns;
      // framestats reports rendered frames; an empty buffer cannot distinguish
      // an app that drew nothing from a source that returned nothing, so the
      // window is uncovered either way and the source status carries which.
      gap.reason = "no_frames_were_reported";
      cov.gaps.push_back(std::move(gap));
    }
    out.coverage.push_back(std::move(cov));
  }
  if (config.memory) {
    std::int64_t points = 0;
    const model::CounterSeries* series = nullptr;
    for (const auto& c : out.counters) {
      points += static_cast<std::int64_t>(c.points.size());
      if (series == nullptr && !c.points.empty()) series = &c;
    }
    model::Coverage cov;
    cov.collector = "dumpsys meminfo";
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = points;
    // meminfo samples instants, not intervals, so a run of them can never
    // claim to have watched the whole window -- the per-source limitation says
    // so. What a gap can honestly mark here is a cadence that slipped: if two
    // consecutive samples are further apart than twice the requested tick,
    // memory went unobserved for a stretch the caller did not ask for.
    if (series == nullptr) {
      model::CoverageGap gap;
      gap.collector = "dumpsys meminfo";
      gap.start_ns = out.window_start_ns;
      gap.end_ns = out.window_end_ns;
      gap.reason = "no_memory_sample_was_recorded";
      cov.gaps.push_back(std::move(gap));
    }
    // A batch capture takes one reading, so there is no cadence to have
    // slipped and no gap to claim from the spacing of a single point.
    if (series != nullptr && check_cadence) {
      const auto tick_ns = static_cast<model::TimeNs>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              config.tick_interval)
              .count());
      const model::TimeNs allowed = tick_ns > 0 ? tick_ns * 2 : 0;
      if (allowed > 0) {
        for (std::size_t i = 1; i < series->points.size(); ++i) {
          const auto from = series->points[i - 1].first;
          const auto to = series->points[i].first;
          if (to - from <= allowed) continue;
          model::CoverageGap gap;
          gap.collector = "dumpsys meminfo";
          gap.start_ns = from;
          gap.end_ns = to;
          gap.reason = "tick_interval_overrun";
          cov.gaps.push_back(std::move(gap));
        }
      }
    }
    out.coverage.push_back(std::move(cov));
  }
}

// Records that a source which did not run covered none of the window.
//
// The collectors already write a coverage row per source, so this merges into
// the row that is there rather than adding a second one: two rows for one
// collector read as two different measurements of the same thing. The
// status-derived reason is the more informative of the two -- it says *why*
// nothing was collected -- so it replaces a generic "produced nothing" gap
// over the same interval instead of sitting beside it.
void record_failed_source_coverage(
    model::NormalizedTrace& out,
    const std::vector<model::Capability>& sources) {
  static const char* generic[] = {"no_sampling_window_completed",
                                  "no_frames_were_reported",
                                  "no_memory_sample_was_recorded"};
  for (const auto& c : sources) {
    if (c.status == model::CapabilityStatus::kAvailable ||
        c.status == model::CapabilityStatus::kLimited) {
      continue;
    }
    model::CoverageGap gap;
    gap.collector = c.provider;
    gap.start_ns = out.window_start_ns;
    gap.end_ns = out.window_end_ns;
    gap.reason = std::string("source_") + model::to_string(c.status);

    model::Coverage* existing = nullptr;
    for (auto& cov : out.coverage) {
      if (cov.collector == c.provider) {
        existing = &cov;
        break;
      }
    }
    if (existing == nullptr) {
      model::Coverage cov;
      cov.collector = c.provider;
      cov.window_start_ns = out.window_start_ns;
      cov.window_end_ns = out.window_end_ns;
      cov.event_count = 0;
      cov.gaps.push_back(std::move(gap));
      out.coverage.push_back(std::move(cov));
      continue;
    }
    std::erase_if(existing->gaps, [&](const model::CoverageGap& g) {
      if (g.start_ns != gap.start_ns || g.end_ns != gap.end_ns) return false;
      for (const char* r : generic) {
        if (g.reason == r) return true;
      }
      return false;
    });
    existing->gaps.push_back(std::move(gap));
  }
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

// Declared in the header for testing.
AmStartResult parse_am_start_w(const std::string& text) {
  AmStartResult r;
  std::optional<model::TimeNs> total;
  std::optional<model::TimeNs> wait;
  bool not_started_warning = false;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (t.empty()) continue;
    const auto colon = t.find(':');
    const std::string key = colon == std::string::npos ? t : trim(t.substr(0, colon));
    const std::string value =
        colon == std::string::npos ? std::string() : trim(t.substr(colon + 1));

    if (key == "Status") {
      r.status = value;
    } else if (key == "LaunchState") {
      r.launch_state = value;
    } else if (key == "Activity") {
      r.component = value;
    } else if (key == "Warning") {
      r.warnings.push_back(value);
      // The one warning that invalidates the numbers rather than qualifying
      // them: nothing was launched, so there is no launch to time.
      if (value.find("Activity not started") != std::string::npos) {
        not_started_warning = true;
      }
    } else if (key == "Error" || key == "Error type") {
      r.warnings.push_back(t);
    } else if (key == "TotalTime") {
      const auto ms = to_i64(value, -1);
      if (ms >= 0) total = ms * 1000000;
    } else if (key == "WaitTime") {
      const auto ms = to_i64(value, -1);
      if (ms >= 0) wait = ms * 1000000;
    }
  }

  r.started = r.status == "ok" && !not_started_warning;
  if (!r.started) {
    r.refusal = not_started_warning
                    ? "the platform reported that no activity was started "
                      "(the intent went to the running top-most instance), so "
                      "its TotalTime of 0 is not a startup duration"
                    : "`am start -W` did not report Status: ok";
    return r;
  }
  // A launch that really happened and really took no measurable time is not a
  // thing the platform reports; a zero here means the same as the warning.
  if (total.has_value() && *total == 0) {
    r.started = false;
    r.refusal =
        "the platform reported TotalTime 0 for a launch it claimed to have "
        "started, which is not a duration this tool will pass on as one";
    return r;
  }
  r.total_time_ns = total;
  r.wait_time_ns = wait;
  return r;
}

std::vector<DisplayedRecord> parse_displayed_log(const std::string& text) {
  std::vector<DisplayedRecord> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto marker = line.find("Displayed ");
    if (marker == std::string::npos) continue;
    const auto plus = line.find(": +", marker);
    if (plus == std::string::npos) continue;

    DisplayedRecord rec;
    // Between "Displayed " and the next space is the component.
    const auto comp_start = marker + std::string("Displayed ").size();
    const auto comp_end = line.find(' ', comp_start);
    if (comp_end == std::string::npos) continue;
    rec.component = line.substr(comp_start, comp_end - comp_start);
    rec.raw = trim(line.substr(plus + 2));

    // "+3s687ms", "+687ms", "+1m2s3ms": accumulate each unit rather than
    // assuming a shape.
    model::TimeNs total = 0;
    std::int64_t digits = 0;
    bool have_digits = false;
    for (std::size_t i = 0; i < rec.raw.size(); ++i) {
      const char c = rec.raw[i];
      if (c >= '0' && c <= '9') {
        digits = digits * 10 + (c - '0');
        have_digits = true;
        continue;
      }
      if (!have_digits) continue;
      if (c == 'm' && i + 1 < rec.raw.size() && rec.raw[i + 1] == 's') {
        total += digits * 1000000;
        ++i;
      } else if (c == 's') {
        total += digits * 1000000000;
      } else if (c == 'm') {
        total += digits * 60ll * 1000000000;
      } else {
        continue;
      }
      digits = 0;
      have_digits = false;
    }
    if (total <= 0) continue;
    rec.elapsed_ns = total;
    out.push_back(std::move(rec));
  }
  return out;
}

std::string parse_resolved_activity(const std::string& text) {
  // The brief form prints the component on its own line; earlier lines are
  // the resolution details. Taking the last line that looks like a component
  // avoids depending on how many detail lines the platform printed.
  std::string found;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    const auto slash = t.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= t.size()) continue;
    if (t.find(' ') != std::string::npos || t.find('=') != std::string::npos) continue;
    found = t;
  }
  return found;
}

bool parse_leading_double(const std::string& text, double& out) {
  const std::string t = trim(text);
  if (t.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (errno != 0 || end == t.c_str() || !std::isfinite(v)) return false;
  out = v;
  return true;
}

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

// ---------------------------------------------------------------------------
// Shared collection steps
//
// The batch and streaming paths call the same helpers, so a frame recorded
// live is byte-for-byte the frame the same capture would have recorded in one
// shot. Only the cadence differs.
// ---------------------------------------------------------------------------

std::int64_t AdbCollector::collect_frames_once(
    const model::DeviceRef& device, const session::CaptureConfig& config,
    model::NormalizedTrace& out, std::vector<std::string>& notes,
    bool& source_ok, std::string& evidence) {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(15000);
  po.cancel = config.cancel;

  const auto r = proc::run(
      shell_argv(device.device_id,
                 {"dumpsys", "gfxinfo", stream_.package, "framestats"}),
      po);
  if (!r.ok()) {
    source_ok = false;
    evidence = "`dumpsys gfxinfo framestats` failed: " +
               (r.spawned ? trim(r.err) : r.spawn_error);
    return 0;
  }

  const FrameStats fs = parse_gfxinfo_framestats(r.out);
  std::int64_t accepted = 0;
  std::int64_t duplicates = 0;
  model::TimeNs highest = stream_.last_frame_vsync;
  const model::TimeNs previous_high = stream_.last_frame_vsync;
  model::TimeNs oldest_accepted = 0;
  for (const auto& row : fs.rows) {
    // The read is a ring buffer, not a queue: rows already recorded come back
    // on the next read too. IntendedVsync identifies a frame, so anything at
    // or below the high-water mark has already been counted.
    if (row.intended_vsync <= stream_.last_frame_vsync) {
      ++duplicates;
      continue;
    }
    highest = std::max(highest, row.intended_vsync);
    if (oldest_accepted == 0 || row.intended_vsync < oldest_accepted) {
      oldest_accepted = row.intended_vsync;
    }
    ++accepted;

    model::FrameRecord fr;
    fr.event_id = "frame-" + std::to_string(stream_.frame_seq++);
    fr.start_ns = row.intended_vsync;
    if (row.frame_deadline > row.intended_vsync) {
      fr.deadline_ns = row.frame_deadline - row.intended_vsync;
    }
    // DisplayPresentTime is the real presentation instant but is zero when the
    // platform does not supply it. FrameCompleted is then the best available,
    // and the source is labelled so nothing downstream calls it presentation
    // truth.
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
    fr.process_instance_id = stream_.process_key;
    fr.surface = stream_.package;
    out.frames.push_back(std::move(fr));
  }

  // The refresh rate comes from the platform's own FrameInterval. Across a
  // streaming capture the intervals accumulate, so more than one distinct
  // value means the rate changed during the session and no single deadline is
  // defensible for the whole window.
  if (!fs.observed_frame_intervals.empty()) {
    for (const auto iv : fs.observed_frame_intervals) {
      const double hz = iv > 0 ? 1e9 / static_cast<double>(iv) : 0.0;
      bool already = false;
      for (const auto& ri : out.refresh_intervals) {
        if (ri.hz.has_value() && std::abs(*ri.hz - hz) < 0.01) already = true;
      }
      if (already || hz <= 0.0) continue;
      model::RefreshInterval ri;
      ri.start_ns = out.frames.empty() ? 0 : out.frames.front().start_ns;
      ri.end_ns = out.frames.empty()
                      ? 0
                      : out.frames.back().presented_ns.value_or(
                            out.frames.back().start_ns);
      ri.hz = hz;
      ri.variable = false;
      ri.provider = "dumpsys gfxinfo FrameInterval";
      out.refresh_intervals.push_back(std::move(ri));
    }
    if (out.refresh_intervals.size() > 1) {
      for (auto& ri : out.refresh_intervals) ri.variable = true;
      notes.push_back(
          "the platform reported more than one frame interval during this "
          "capture, so the refresh rate is variable and no single deadline "
          "applies to the whole window");
    }
  }

  stream_.last_frame_vsync = highest;
  stream_.frames_seen_duplicate += duplicates;

  // A read that was *entirely* duplicates means the app rendered nothing since
  // the previous tick. A read where every row is new, and the row count is at
  // the ring buffer's capacity, means frames were produced faster than the
  // tick could collect them -- which is a real loss and is reported.
  if (accepted > 0 && duplicates == 0 && fs.rows.size() >= 120) {
    notes.push_back(
        "the frame buffer was full and contained no frame seen before, so "
        "frames rendered between ticks were lost: shorten the tick interval");
    // The lost stretch is known: it runs from the last frame this capture did
    // see to the oldest one this read returned.
    if (previous_high > 0 && oldest_accepted > previous_high) {
      model::CoverageGap gap;
      gap.collector = "dumpsys gfxinfo";
      gap.start_ns = previous_high;
      gap.end_ns = oldest_accepted;
      gap.reason = "frame_ring_buffer_wrapped_between_ticks";
      stream_.frame_gaps.push_back(std::move(gap));
    }
  }

  for (const auto& w : fs.warnings) notes.push_back(w);
  source_ok = true;
  evidence = "framestats returned " + std::to_string(fs.rows.size()) +
             " row(s), " + std::to_string(accepted) + " new";
  return accepted;
}

std::int64_t AdbCollector::collect_memory_once(
    const model::DeviceRef& device, const session::CaptureConfig& config,
    model::TimeNs at_ns, model::NormalizedTrace& out, bool& source_ok,
    std::string& evidence) {
  proc::Options po;
  po.timeout = std::chrono::milliseconds(15000);
  po.cancel = config.cancel;

  const auto r = proc::run(
      shell_argv(device.device_id, {"dumpsys", "meminfo", stream_.package}), po);
  if (!r.ok()) {
    source_ok = false;
    evidence = "`dumpsys meminfo` failed: " +
               (r.spawned ? trim(r.err) : r.spawn_error);
    return 0;
  }

  const MemInfo mem = parse_dumpsys_meminfo(r.out);
  std::int64_t added = 0;
  // Each family keeps its own series and is appended to, so repeated ticks
  // build a time series rather than a pile of one-point series. Spec section 8
  // forbids summing or equating the families, so they never merge.
  const struct {
    const char* name;
    const char* family;
    const std::optional<double>& value;
  } series[] = {
      {"memory.rss_total_bytes", "rss", mem.rss_total_bytes},
      {"memory.pss_total_bytes", "pss", mem.pss_total_bytes},
      {"memory.private_dirty_bytes", "private_dirty", mem.private_dirty_bytes},
      {"memory.native_heap_rss_bytes", "native_heap", mem.native_heap_rss_bytes},
      {"memory.dalvik_heap_rss_bytes", "dalvik_heap", mem.dalvik_heap_rss_bytes},
  };
  for (const auto& spec : series) {
    if (!spec.value.has_value()) continue;
    model::CounterSeries* target = nullptr;
    for (auto& c : out.counters) {
      if (c.name == spec.name) {
        target = &c;
        break;
      }
    }
    if (target == nullptr) {
      model::CounterSeries c;
      c.name = spec.name;
      c.unit = "bytes";
      c.provider = "dumpsys meminfo";
      c.process_instance_id = stream_.process_key;
      c.family = spec.family;
      out.counters.push_back(std::move(c));
      target = &out.counters.back();
    }
    target->points.emplace_back(at_ns, *spec.value);
    ++added;
  }
  source_ok = true;
  evidence = "meminfo yielded " + std::to_string(added) + " counter point(s)";
  return added;
}

SimpleperfSamples AdbCollector::record_cpu_window(
    const model::DeviceRef& device, const session::CaptureConfig& config,
    std::chrono::milliseconds window, bool& ok, std::string& error) {
  SimpleperfSamples empty;
  const std::string perf_path = "/data/local/tmp/mpi-perf.data";
  proc::Options po;
  po.timeout = window + std::chrono::milliseconds(45000);
  // Deliberately not config.cancel: a window already recording is allowed to
  // finish so its samples are not thrown away. stop_cpu_worker() stops the
  // loop between windows instead.
  po.cancel = CancellationToken::none();

  const long long seconds = std::max<long long>(1, window.count() / 1000);
  const auto rec = proc::run(
      shell_argv(device.device_id,
                 {"simpleperf", "record", "--app", stream_.package, "-o",
                  perf_path, "--duration", std::to_string(seconds), "-f",
                  std::to_string(config.sample_frequency_hz), "-g", "-e",
                  "cpu-clock"}),
      po);
  if (!rec.ok()) {
    ok = false;
    error = rec.spawned ? trim(rec.err) : rec.spawn_error;
    return empty;
  }

  const auto rep = proc::run(
      shell_argv(device.device_id, {"simpleperf", "report-sample",
                                    "--show-callchain", "-i", perf_path}),
      po);
  // Remove only the file this session created (spec D22).
  proc::run(shell_argv(device.device_id, {"rm", "-f", perf_path}), po);

  if (!rep.ok()) {
    ok = false;
    error = "report-sample failed: " +
            (rep.spawned ? trim(rep.err) : rep.spawn_error);
    return empty;
  }
  ok = true;
  error.clear();
  return parse_simpleperf_report_sample(rep.out);
}

void AdbCollector::start_cpu_worker(const model::DeviceRef& device,
                                    const session::CaptureConfig& config) {
  stream_.cpu_stop.store(false);
  stream_.cpu_thread = std::thread([this, device, config] {
    while (!stream_.cpu_stop.load()) {
      const auto window_started = std::chrono::steady_clock::now();
      bool ok = false;
      std::string error;
      auto parsed = record_cpu_window(device, config, config.cpu_window, ok, error);
      const auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - window_started);

      std::lock_guard<std::mutex> lock(stream_.cpu_mutex);
      stream_.cpu_busy += cost;
      ++stream_.cpu_windows_done;
      if (!ok) {
        stream_.cpu_ok = false;
        stream_.cpu_status_error = error;
        stream_.cpu_denied =
            error.find("Permission denied") != std::string::npos ||
            error.find("isn't debuggable") != std::string::npos ||
            error.find("not debuggable") != std::string::npos ||
            error.find("debuggable/profileable") != std::string::npos;
        // A target that cannot be profiled will not become profileable by
        // retrying, so the worker stops rather than spinning on it.
        if (stream_.cpu_denied) return;
        continue;
      }
      stream_.cpu_ok = true;
      if (!stream_.meta_recorded && !parsed.app_type.empty()) {
        stream_.pending_meta = parsed;
        stream_.meta_recorded = true;
      }
      if (!parsed.samples.empty()) {
        model::TimeNs lo = parsed.samples.front().time_ns;
        model::TimeNs hi = lo;
        for (const auto& smp : parsed.samples) {
          lo = std::min(lo, smp.time_ns);
          hi = std::max(hi, smp.time_ns);
        }
        stream_.cpu_covered.emplace_back(lo, hi);
      }
      stream_.pending_samples.insert(stream_.pending_samples.end(),
                                     parsed.samples.begin(),
                                     parsed.samples.end());
    }
  });
}

void AdbCollector::stop_cpu_worker() {
  stream_.cpu_stop.store(true);
  if (stream_.cpu_thread.joinable()) stream_.cpu_thread.join();
}

std::int64_t AdbCollector::drain_cpu_samples(model::NormalizedTrace& out) {
  std::vector<SimpleperfSamples::Sample> batch;
  SimpleperfSamples meta;
  bool have_meta = false;
  {
    std::lock_guard<std::mutex> lock(stream_.cpu_mutex);
    batch.swap(stream_.pending_samples);
    if (stream_.meta_recorded && !stream_.pending_meta.app_type.empty()) {
      meta = stream_.pending_meta;
      have_meta = true;
      stream_.pending_meta = SimpleperfSamples{};
    }
  }

  for (const auto& sample : batch) {
    const std::string tkey =
        stream_.process_key + "|tid=" + std::to_string(sample.thread_id);
    bool known = false;
    for (const auto& t : out.threads) {
      if (t.thread_instance_id == tkey) known = true;
    }
    if (!known) {
      model::ThreadInfo ti;
      ti.thread_instance_id = tkey;
      ti.process_instance_id = stream_.process_key;
      ti.tid = sample.thread_id;
      ti.name = sample.thread_name;
      ti.is_main_ui_thread =
          sample.thread_name == stream_.package || sample.thread_name == "main";
      // Thread names observed on a real React Native debug build: the JS
      // thread is "mqt_v_js" there, and older builds use "mqt_js".
      ti.is_js_thread = sample.thread_name == "mqt_js" ||
                        sample.thread_name == "mqt_v_js" ||
                        sample.thread_name.rfind("mqt_js", 0) == 0 ||
                        sample.thread_name.find("hermes") != std::string::npos ||
                        sample.thread_name.find("Hermes") != std::string::npos;
      out.threads.push_back(std::move(ti));
    }

    model::CpuSample smp;
    smp.timestamp_ns = sample.time_ns;
    smp.process_instance_id = stream_.process_key;
    smp.thread_instance_id = tkey;
    smp.provider = "simpleperf";
    smp.weight = sample.event_count > 0 ? std::optional<double>(1.0) : std::nullopt;
    smp.frames = sample.frames;
    out.cpu_samples.push_back(std::move(smp));

    model::Event ev;
    ev.event_id = "simpleperf-" + std::to_string(stream_.sample_seq++);
    ev.provider = "simpleperf";
    ev.clock_domain = stream_.clock_id;
    ev.timestamp = sample.time_ns;
    ev.process_instance_id = stream_.process_key;
    ev.thread_instance_id = tkey;
    ev.category = model::EventCategory::kCpuSample;
    ev.name = out.cpu_samples.back().frames.empty()
                  ? std::string("(unsymbolised)")
                  : out.cpu_samples.back().frames.back();
    out.events.push_back(std::move(ev));
  }

  // Build facts read off the device, recorded once.
  if (have_meta) {
    if (out.build.find("native.debuggable") == nullptr) {
      model::BuildFact f;
      f.key = "native.debuggable";
      f.value = meta.app_type;
      f.boolean_value =
          meta.app_type == "debuggable" ? model::Tri::kTrue : model::Tri::kFalse;
      f.source = model::FactSource::kDeviceProvider;
      f.observed_at = time_util::now_iso8601_utc();
      f.basis = "simpleperf meta_info app_type=" + meta.app_type;
      out.build.upsert(std::move(f));
    }
    if (!meta.build_type.empty() &&
        out.build.find("device.build_type") == nullptr) {
      model::BuildFact f;
      f.key = "device.build_type";
      f.value = meta.build_type;
      f.source = model::FactSource::kDeviceProvider;
      f.observed_at = time_util::now_iso8601_utc();
      f.basis = "simpleperf meta_info android_build_type";
      out.build.upsert(std::move(f));
    }
    if (meta.sdk_version.has_value() &&
        out.build.find("device.sdk_version") == nullptr) {
      model::BuildFact f;
      f.key = "device.sdk_version";
      f.value = std::to_string(*meta.sdk_version);
      f.source = model::FactSource::kDeviceProvider;
      f.observed_at = time_util::now_iso8601_utc();
      f.basis = "simpleperf meta_info android_sdk_version";
      out.build.upsert(std::move(f));
    }
  }
  return static_cast<std::int64_t>(batch.size());
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Launch and startup measurement
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Scheduling and I/O (atrace)
// ---------------------------------------------------------------------------

std::string AdbCollector::collect_heap_dump(
    const model::DeviceRef& device, const session::CaptureConfig& config,
    std::int32_t pid, model::Capability& capability) {
  proc::Options po;
  po.cancel = config.cancel;

  if (config.artifact_dir.empty()) {
    capability.status = model::CapabilityStatus::kUnsupported;
    capability.evidence =
        "no directory was provided to write the dump into, so nothing was "
        "requested from the device";
    return {};
  }

  const std::string pid_text = std::to_string(pid);
  // A path under /data/local/tmp, which the shell user can write and read
  // back. The app's own directory is not usable: the shell cannot read it on
  // a non-debuggable app, and `am dumpheap` writes as the app.
  const std::string remote =
      "/data/local/tmp/mpi-heap-" + pid_text + ".hprof";
  static_cast<void>(proc::run(
      shell_argv(device.device_id, {"rm", "-f", remote}), po));

  // `am dumpheap` returns as soon as the request is made; the runtime walks
  // the heap afterwards. Its "Waiting for dump to finish..." is printed by am
  // itself and does not mean the file is complete, so completion is decided
  // by watching the size settle rather than by trusting the exit.
  po.timeout = std::chrono::milliseconds(120000);
  const auto request = proc::run(
      shell_argv(device.device_id, {"am", "dumpheap", pid_text, remote}), po);
  if (!request.ok()) {
    capability.status = model::CapabilityStatus::kPermissionDenied;
    capability.evidence =
        "`am dumpheap` was refused: " +
        trim(request.err.empty() ? request.out : request.err);
    capability.prerequisites.push_back(
        "the target must be debuggable, or the device must run a userdebug "
        "build; a release app on a user build cannot be dumped");
    capability.recovery_action =
        "install a debuggable build of the app, or use a userdebug device";
    return {};
  }

  std::int64_t size = 0;
  std::int64_t stable_for = 0;
  po.timeout = std::chrono::milliseconds(10000);
  for (int i = 0; i < 120; ++i) {
    if (config.cancel.cancelled()) break;
    const auto stat = proc::run(
        shell_argv(device.device_id, {"stat", "-c", "%s", remote}), po);
    std::int64_t now = 0;
    if (stat.ok()) {
      now = std::atoll(trim(stat.out).c_str());
    }
    // Two consecutive equal, non-zero sizes: the runtime has stopped writing.
    if (now > 0 && now == size) {
      if (++stable_for >= 2) break;
    } else {
      stable_for = 0;
    }
    size = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (size <= 0) {
    capability.status = model::CapabilityStatus::kUnsupported;
    capability.evidence =
        "the dump file never appeared on the device, so no heap was captured";
    return {};
  }

  const std::string local = config.artifact_dir + "/heap.hprof";
  po.timeout = std::chrono::milliseconds(180000);
  const auto pull = proc::run(
      {adb_path_, "-s", device.device_id, "pull", remote, local}, po);
  static_cast<void>(proc::run(
      shell_argv(device.device_id, {"rm", "-f", remote}), po));
  if (!pull.ok()) {
    capability.status = model::CapabilityStatus::kUnsupported;
    capability.evidence = "the dump could not be pulled from the device: " +
                          trim(pull.err);
    return {};
  }

  capability.status = model::CapabilityStatus::kAvailable;
  capability.evidence = "`am dumpheap` wrote " +
                        std::to_string(size / (1024 * 1024)) +
                        " MiB, pulled to the session";
  capability.limitations.push_back(
      "a heap dump is one instant: it shows what was reachable then, and "
      "nothing about how long any object had been alive");
  capability.limitations.push_back(
      "the runtime collects garbage before writing the dump, but an object on "
      "a finalizer or reference queue is still present for one more cycle, so "
      "presence is not retention");
  capability.limitations.push_back(
      "the app is paused while its heap is walked, so any timing measured "
      "across this point includes the pause");
  return local;
}

std::int64_t AdbCollector::collect_scheduling(
    const model::DeviceRef& device, const session::CaptureConfig& config,
    std::int32_t pid, model::NormalizedTrace& out,
    model::Capability& capability) {
  proc::Options po;
  po.cancel = config.cancel;

  // The app's own threads. atrace is system-wide, so without this list there
  // is nothing to attribute: every other line in the file belongs to another
  // process and must stay out of the app's totals.
  const std::string pid_text = std::to_string(pid);
  std::vector<AppThread> threads;
  {
    po.timeout = std::chrono::milliseconds(15000);
    const auto listing = proc::run(
        shell_argv(device.device_id,
                   {"ls", "/proc/" + pid_text + "/task"}),
        po);
    if (!listing.ok()) {
      capability.status = model::CapabilityStatus::kUnsupported;
      capability.evidence =
          "the target's thread list could not be read (/proc/" + pid_text +
          "/task): a system-wide trace cannot be attributed to the app "
          "without it, and attributing it anyway would credit other "
          "processes' work to this one";
      return 0;
    }
    std::istringstream tids(listing.out);
    std::string tid_line;
    while (std::getline(tids, tid_line)) {
      const auto tid = to_i64(trim(tid_line), 0);
      if (tid <= 0) continue;
      AppThread thread;
      thread.tid = static_cast<std::int32_t>(tid);
      const auto comm = proc::run(
          shell_argv(device.device_id,
                     {"cat", "/proc/" + pid_text + "/task/" +
                                 std::to_string(tid) + "/comm"}),
          po);
      if (comm.ok()) thread.name = trim(comm.out);
      threads.push_back(std::move(thread));
    }
  }
  if (threads.empty()) {
    capability.status = model::CapabilityStatus::kLimited;
    capability.evidence = "the target reported no threads, so nothing in a "
                          "system-wide trace could be attributed to it";
    return 0;
  }

  const auto seconds = std::max<std::int64_t>(
      1, std::chrono::duration_cast<std::chrono::seconds>(config.duration)
             .count());
  po.timeout = std::chrono::milliseconds((seconds + 30) * 1000);
  const auto traced = proc::run(
      shell_argv(device.device_id,
                 {"atrace", "-t", std::to_string(seconds), "-b",
                  std::to_string(config.scheduling_buffer_kb), "sched", "disk",
                  "am", "view"}),
      po);
  if (!traced.ok()) {
    capability.status = model::CapabilityStatus::kUnsupported;
    capability.evidence =
        "`atrace` failed: " +
        (traced.spawned ? trim(traced.err) : traced.spawn_error);
    return 0;
  }

  const auto trace = parse_atrace(traced.out);
  for (const auto& w : trace.warnings) capability.limitations.push_back(w);

  const std::string process_key =
      stream_.process_key.empty()
          ? (out.target.processes.empty() ? std::string()
                                         : out.target.processes.front().canonical())
          : stream_.process_key;

  const auto mapping =
      map_atrace_to_trace(trace, threads, pid, process_key, out);
  const std::int64_t emitted = mapping.events_emitted;
  const std::int64_t foreign_lines = mapping.foreign_events;

  if (trace.dropped_events > 0) {
    out.dropped_events_by_collector["atrace"] += trace.dropped_events;
  }

  capability.status = emitted > 0 ? model::CapabilityStatus::kAvailable
                                  : model::CapabilityStatus::kLimited;
  capability.evidence =
      "atrace read " + std::to_string(trace.lines_read) + " line(s) and " +
      std::to_string(emitted) + " event(s) were attributed to " +
      std::to_string(threads.size()) + " thread(s) of pid " + pid_text;
  capability.limitations.push_back(
      "the trace is system-wide: " + std::to_string(foreign_lines) +
      " line(s) belonged to other processes and are out of scope for this "
      "app rather than counted as its idle time");
  capability.limitations.push_back(
      "a `D` state means uninterruptible sleep, which is usually disk but is "
      "only I/O when the kernel also reported iowait=1");
  capability.limitations.push_back(
      "no block-device events were requested or parsed, so an I/O wait is "
      "located by its kernel caller and not by file or size");
  if (trace.lines_unrecognised > 0) {
    capability.limitations.push_back(
        std::to_string(trace.lines_unrecognised) +
        " line(s) could not be parsed and were counted rather than guessed at");
  }
  return emitted;
}

std::optional<session::Collector::DeviceClock> AdbCollector::device_clock_at(
    std::chrono::steady_clock::time_point host_instant) const {
  if (!stream_.clock_anchored) return std::nullopt;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           host_instant - stream_.host_base)
                           .count();
  session::Collector::DeviceClock clock;
  clock.domain = stream_.clock_id.empty() ? "android.boottime.ns"
                                          : stream_.clock_id;
  clock.at_ns = stream_.boot_base_ns + static_cast<model::TimeNs>(elapsed);
  // The anchor itself came from /proc/uptime, which reports hundredths of a
  // second, and the adb round trip that read it is unmeasured.
  clock.uncertainty_ns = StreamState::kUptimeResolutionNs;
  return clock;
}

session::Collector::LaunchReport AdbCollector::launch(
    const model::DeviceRef& device, const std::string& app_identifier,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::Collector::LaunchReport report;
  report.supported = true;

  if (!proc::is_safe_argument(app_identifier, /*reject_option_like=*/true)) {
    report.error = "refusing to launch identifier '" + app_identifier +
                   "': it would be read as a command-line option";
    return report;
  }

  proc::Options po;
  po.timeout = std::chrono::milliseconds(60000);
  po.cancel = config.cancel;

  const auto source = [&](model::CapabilityStatus status,
                          const std::string& evidence,
                          std::vector<std::string> limitations = {}) {
    auto c = make_source("android.capture.startup", "App startup timing",
                         status, "am start -W");
    c.evidence = evidence;
    c.limitations = std::move(limitations);
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    report.source_results.push_back(std::move(c));
  };

  // The launchable component has to come from the platform: guessing
  // `package/.MainActivity` is wrong for most real apps.
  const auto resolved = proc::run(
      shell_argv(device.device_id,
                 {"cmd", "package", "resolve-activity", "--brief",
                  app_identifier}),
      po);
  const std::string component =
      resolved.ok() ? parse_resolved_activity(resolved.out) : std::string();
  if (component.empty()) {
    report.error = "no launchable activity could be resolved for '" +
                   app_identifier +
                   "'; the package may declare no launcher entry point";
    source(model::CapabilityStatus::kUnsupported, report.error);
    return report;
  }

  const bool cold = config.launch_class == "cold";
  if (cold) {
    // Only a force-stop makes the next launch genuinely cold. Whether it
    // actually was is still read back from the platform, not assumed here.
    proc::run(shell_argv(device.device_id, {"am", "force-stop", app_identifier}),
              po);
  }
  // Clearing the log first so the Displayed line read afterwards belongs to
  // this launch and not to an earlier one.
  proc::run({"adb", "-s", device.device_id, "logcat", "-c"}, po);

  // The device clock is read immediately before the launch so the marker sits
  // on the same timeline as the rest of the capture.
  model::TimeNs launch_at = 0;
  {
    const auto up = proc::run(
        shell_argv(device.device_id, {"cat", "/proc/uptime"}), po);
    double seconds = 0.0;
    if (up.ok() && parse_leading_double(up.out, seconds) && seconds > 0.0) {
      launch_at = static_cast<model::TimeNs>(seconds * 1'000'000'000.0);
    }
  }

  const auto started = proc::run(
      shell_argv(device.device_id, {"am", "start", "-W", "-n", component}), po);
  if (!started.ok()) {
    report.error = "`am start -W` failed: " +
                   (started.spawned ? trim(started.err) : started.spawn_error);
    source(model::CapabilityStatus::kUnsupported, report.error);
    return report;
  }

  const auto parsed = parse_am_start_w(started.out);
  report.launch_class = parsed.launch_state;
  for (const auto& w : parsed.warnings) report.notes.push_back(w);

  if (!parsed.started) {
    // The app is up -- the intent was delivered -- but no launch was timed.
    // Reporting that plainly is the whole point: the platform's zero is not a
    // startup duration.
    report.started = false;
    report.error = parsed.refusal;
    source(model::CapabilityStatus::kLimited, parsed.refusal,
           {"the app is running, but this launch produced no startup "
            "measurement; force-stop it first or record with "
            "--launch-class=cold for a cold start"});
    return report;
  }

  report.started = true;
  report.total_time_ns = parsed.total_time_ns;
  report.wait_time_ns = parsed.wait_time_ns;

  // The platform's own first-frame figure, which is a different endpoint from
  // TotalTime and is kept as its own marker rather than averaged with it.
  const auto log = proc::run(
      {"adb", "-s", device.device_id, "logcat", "-d", "-s",
       "ActivityTaskManager"},
      po);
  if (log.ok()) {
    for (const auto& rec : parse_displayed_log(log.out)) {
      if (rec.component != component) continue;
      report.displayed_ns = rec.elapsed_ns;
    }
  }

  const auto add_marker = [&](const char* kind, model::TimeNs duration,
                              const char* endpoint, const char* provider) {
    model::Marker m;
    m.event_id = std::string("startup-") + kind;
    m.timestamp_ns = launch_at;
    m.duration_ns = duration;
    m.kind = kind;
    m.payload = json::Value::object();
    m.payload.set("component", json::Value::string(component));
    m.payload.set("launch_class_requested",
                  json::Value::string(config.launch_class));
    m.payload.set("launch_class_reported",
                  json::Value::string(parsed.launch_state));
    m.payload.set("endpoint", json::Value::string(endpoint));
    m.payload.set("provider", json::Value::string(provider));
    out.markers.push_back(std::move(m));
  };

  if (report.total_time_ns.has_value()) {
    add_marker("app_launch", *report.total_time_ns,
               "the activity reported being drawn", "am start -W TotalTime");
  }
  if (report.displayed_ns.has_value()) {
    add_marker("startup_displayed", *report.displayed_ns,
               "the platform logged the first frame as displayed",
               "ActivityTaskManager Displayed");
  }

  std::string evidence = "launched " + component + ", platform reported " +
                         parsed.launch_state;
  if (report.total_time_ns.has_value()) {
    evidence += ", TotalTime " +
                std::to_string(*report.total_time_ns / 1000000) + " ms";
  }
  if (report.displayed_ns.has_value()) {
    evidence += ", Displayed " +
                std::to_string(*report.displayed_ns / 1000000) + " ms";
  }
  source(model::CapabilityStatus::kAvailable, evidence,
         {"TotalTime ends when the activity reported being drawn, which is "
          "not when the app became interactive",
          "a single launch is one sample; a startup claim needs repeated runs "
          "(spec section 12)"});
  return report;
}

session::CaptureResult AdbCollector::begin(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::CaptureResult result;
  // StreamState owns a thread and a mutex, so it is reset field-by-field
  // rather than reassigned. Any previous worker is stopped first: leaving one
  // running would have it appending into a trace this session no longer owns.
  stop_cpu_worker();
  stream_.reset();

  if (processes.empty()) {
    result.error = "no process instance was supplied; nothing to capture";
    return result;
  }
  stream_.package = processes.front().app.app_identifier;
  if (!proc::is_safe_argument(stream_.package, /*reject_option_like=*/true)) {
    result.error = "refusing to capture identifier '" + stream_.package +
                   "': it would be read as a command-line option";
    return result;
  }
  const model::ProcessInstance* primary = &processes.front();
  for (const auto& p : processes) {
    if (p.is_primary) primary = &p;
  }
  stream_.process_key = primary->canonical();
  stream_.clock_id = "android.boottime.ns";

  out.device = device;
  if (out.primary_clock_domain.empty()) out.primary_clock_domain = stream_.clock_id;
  model::ClockDomain cd;
  cd.id = stream_.clock_id;
  cd.base = "monotonic";
  cd.provider = "android.kernel.boottime";
  cd.monotonic = true;
  out.clock_domains.push_back(cd);

  proc::Options po;
  po.timeout = std::chrono::milliseconds(15000);
  po.cancel = config.cancel;

  // Anchor the host clock to the device's boot time once, so a source that
  // reports no time of its own (`dumpsys meminfo`) can still be placed on the
  // device clock. /proc/uptime's first field is the boot time in seconds.
  {
    const auto r =
        proc::run(shell_argv(device.device_id, {"cat", "/proc/uptime"}), po);
    double seconds = 0.0;
    if (r.ok() && parse_leading_double(r.out, seconds) && seconds > 0.0) {
      stream_.boot_base_ns =
          static_cast<model::TimeNs>(seconds * 1'000'000'000.0);
      stream_.host_base = std::chrono::steady_clock::now();
      stream_.clock_anchored = true;

      model::ClockMapping m;
      m.from_domain = "host.steady.ns";
      m.to_domain = stream_.clock_id;
      m.offset_ns = stream_.boot_base_ns;
      // /proc/uptime is reported in hundredths of a second, and the adb round
      // trip is itself unmeasured, so the half-width is at least that coarse.
      m.uncertainty_ns = StreamState::kUptimeResolutionNs;
      m.method = "single read of /proc/uptime over adb, paired with the host "
                 "steady clock; later instants are extrapolated from the host";
      m.measured = true;
      out.clock_mappings.push_back(std::move(m));
    }
    // A failed read is not fatal: everything else carries the platform's own
    // timestamps. The memory source reports the consequence per tick.
  }

  if (config.frames && config.reset_frame_history) {
    const auto r = proc::run(
        shell_argv(device.device_id,
                   {"dumpsys", "gfxinfo", stream_.package, "reset"}),
        po);
    stream_.frames_reset = r.ok();
  }

  if (config.cpu_samples) start_cpu_worker(device, config);

  result.started = true;
  return result;
}

// The capture window is whatever the evidence spans. It is recomputed after
// every tick and again once the sampler's last window has been drained: the
// final CPU samples arrive only in finish(), and a window that stopped at the
// last tick left them outside the interval the report says they came from.
void AdbCollector::extend_window_from_trace(model::NormalizedTrace& out) {
  const auto extend = [&](model::TimeNs a, model::TimeNs b) {
    if (a <= 0) return;
    if (!stream_.have_window) {
      stream_.window_lo = a;
      stream_.window_hi = b;
      stream_.have_window = true;
    } else {
      stream_.window_lo = std::min(stream_.window_lo, a);
      stream_.window_hi = std::max(stream_.window_hi, b);
    }
  };
  for (const auto& f : out.frames) {
    extend(f.start_ns, f.presented_ns.value_or(f.start_ns));
  }
  for (const auto& smp : out.cpu_samples) {
    extend(smp.timestamp_ns, smp.timestamp_ns);
  }
  // Counter points count too. Leaving them out put memory samples outside the
  // very window the report says they were collected in, which reads as though
  // the tool had measured something it was not looking at.
  for (const auto& c : out.counters) {
    for (const auto& pt : c.points) extend(pt.first, pt.first);
  }
  if (stream_.have_window) {
    out.window_start_ns = stream_.window_lo;
    out.window_end_ns = stream_.window_hi;
  }
}

session::LiveUpdate AdbCollector::tick(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  session::LiveUpdate update;
  static_cast<void>(processes);
  const auto tick_started = std::chrono::steady_clock::now();
  ++stream_.tick_count;

  // The cheap sources run on the tick. CPU is not one of them: simpleperf
  // costs roughly 5.6 s of fixed overhead per record-and-symbolise cycle, so
  // it runs on its own thread in long windows and the tick only collects
  // whatever that thread has finished.
  if (config.cpu_samples) {
    update.new_cpu_samples = drain_cpu_samples(out);

    std::int64_t windows = 0;
    std::chrono::milliseconds busy{0};
    bool ok = true;
    bool denied = false;
    std::string error;
    {
      std::lock_guard<std::mutex> lock(stream_.cpu_mutex);
      windows = stream_.cpu_windows_done;
      busy = stream_.cpu_busy;
      ok = stream_.cpu_ok;
      denied = stream_.cpu_denied;
      error = stream_.cpu_status_error;
    }

    auto c = make_source("android.capture.cpu_samples", "Sampled CPU stacks",
                         ok ? (out.cpu_samples.empty()
                                   ? model::CapabilityStatus::kLimited
                                   : model::CapabilityStatus::kAvailable)
                            : (denied ? model::CapabilityStatus::kPermissionDenied
                                      : model::CapabilityStatus::kUnsupported),
                         "simpleperf");
    if (ok) {
      c.evidence = std::to_string(windows) + " window(s) of " +
                   std::to_string(config.cpu_window.count() / 1000) +
                   "s completed; " + std::to_string(out.cpu_samples.size()) +
                   " sample(s) so far";
      c.limitations.push_back(
          "sampled in windows on a background thread, not on the tick: CPU "
          "numbers lag the frame and memory numbers by up to one window plus "
          "its symbolisation time");
      if (windows == 0) {
        c.limitations.push_back(
            "the first window has not completed yet, so no CPU sample has "
            "arrived; this is not an absence of CPU activity");
      }
    } else {
      c.evidence = "simpleperf could not record: " + error;
      c.recovery_action =
          denied ? "this target is not debuggable or profileable, which is "
                   "Android's own restriction and is not bypassed here"
                 : "check that simpleperf is present and the device is authorized";
    }
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    update.source_status.push_back(std::move(c));
    static_cast<void>(busy);
  }

  if (config.frames) {
    std::string evidence;
    bool ok = true;
    // The read returns a ring buffer that overlaps the previous read, so
    // collect_frames_once de-duplicates by IntendedVsync; `added` is the
    // genuinely new frames.
    const auto added = collect_frames_once(device, config, out, update.notes,
                                           ok, evidence);
    update.new_frames = added;
    stream_.frames_ok = ok;
    auto c = make_source("android.capture.frames", "Frame timing",
                         ok ? (added > 0 ? model::CapabilityStatus::kAvailable
                                         : model::CapabilityStatus::kLimited)
                            : model::CapabilityStatus::kUnsupported,
                         "dumpsys gfxinfo");
    c.evidence = evidence;
    if (!stream_.frames_reset && config.reset_frame_history) {
      c.limitations.push_back(
          "the frame history could not be reset at capture start");
    }
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    update.source_status.push_back(std::move(c));
  }

  if (config.memory) {
    // Placed at the tick's own instant, which is what makes repeated ticks a
    // time series rather than a set of unrelated readings. The instant comes
    // from the clock anchor taken at capture start; without it there is no
    // honest timestamp to give a meminfo reading, and the samples are dropped
    // rather than stamped with an unrelated event's time or with zero.
    const model::TimeNs at =
        stream_.clock_anchored ? stream_.device_now_ns() : 0;
    std::string evidence;
    bool ok = true;
    std::int64_t added = 0;
    if (stream_.clock_anchored) {
      added = collect_memory_once(device, config, at, out, ok, evidence);
    } else {
      ok = false;
      evidence = "the device clock could not be read at capture start "
                 "(/proc/uptime), so a meminfo reading cannot be placed on the "
                 "capture timeline; no memory points were recorded";
    }
    update.new_counter_points = added;
    stream_.memory_ok = ok;
    auto c = make_source("android.capture.memory", "Process memory counters",
                         ok ? model::CapabilityStatus::kAvailable
                            : model::CapabilityStatus::kUnsupported,
                         "dumpsys meminfo");
    c.evidence = evidence;
    c.limitations.push_back(
        "sampled once per tick, so growth between ticks is not observed");
    if (stream_.clock_anchored) {
      c.limitations.push_back(
          "meminfo carries no timestamp of its own: each point is placed by "
          "the host clock against a single reading of the device boot time, "
          "so its position on the timeline is accurate to about 10 ms plus "
          "clock drift, not exact");
    }
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    update.source_status.push_back(std::move(c));
  }

  extend_window_from_trace(out);

  update.at_ns = stream_.window_hi;
  update.tick_cost = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - tick_started);
  stream_.total_tick_cost += update.tick_cost;
  return update;
}

session::CaptureResult AdbCollector::finish(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  static_cast<void>(processes);
  session::CaptureResult result;
  result.started = true;

  // Stop the sampler and take whatever its last completed window produced.
  if (config.cpu_samples) {
    stop_cpu_worker();
    drain_cpu_samples(out);
    // Coverage below is measured against the window, so the window has to
    // account for these samples before it is used.
    extend_window_from_trace(out);

    // The intervals between sampling windows are genuine gaps. Recording them
    // is what stops an unsampled stretch from reading as measured idle time
    // (spec section 8, E13).
    std::vector<std::pair<model::TimeNs, model::TimeNs>> covered;
    {
      std::lock_guard<std::mutex> lock(stream_.cpu_mutex);
      covered = stream_.cpu_covered;
    }
    std::sort(covered.begin(), covered.end());
    model::Coverage cov;
    cov.collector = "simpleperf";
    cov.window_start_ns = out.window_start_ns;
    cov.window_end_ns = out.window_end_ns;
    cov.event_count = static_cast<std::int64_t>(out.cpu_samples.size());
    model::TimeNs cursor = out.window_start_ns;
    for (const auto& span : covered) {
      if (span.first > cursor) {
        model::CoverageGap gap;
        gap.collector = "simpleperf";
        gap.start_ns = cursor;
        gap.end_ns = span.first;
        gap.reason = "between_sampling_windows";
        cov.gaps.push_back(std::move(gap));
      }
      cursor = std::max(cursor, span.second);
    }
    if (out.window_end_ns > cursor && !covered.empty()) {
      model::CoverageGap gap;
      gap.collector = "simpleperf";
      gap.start_ns = cursor;
      gap.end_ns = out.window_end_ns;
      gap.reason = "after_last_sampling_window";
      cov.gaps.push_back(std::move(gap));
    }
    if (covered.empty()) {
      // No sampling window completed at all. Without a gap this row reported
      // full coverage of a window it never observed, which is the one reading
      // the report must never allow: nothing measured is not nothing there.
      model::CoverageGap gap;
      gap.collector = "simpleperf";
      gap.start_ns = out.window_start_ns;
      gap.end_ns = out.window_end_ns;
      gap.reason = "no_sampling_window_completed";
      cov.gaps.push_back(std::move(gap));
    }
    out.coverage.push_back(std::move(cov));
  }
  record_tick_source_coverage(out, config, stream_.frame_gaps,
                              /*check_cadence=*/true);

  result.any_data = !out.frames.empty() || !out.cpu_samples.empty() ||
                    !out.counters.empty();
  result.elapsed = stream_.total_tick_cost;

  auto summarise = [&](const char* id, const char* human, const char* provider,
                       bool ok, std::size_t count, const char* unit) {
    auto c = make_source(id, human,
                         ok ? (count > 0 ? model::CapabilityStatus::kAvailable
                                         : model::CapabilityStatus::kLimited)
                            : model::CapabilityStatus::kUnsupported,
                         provider);
    c.evidence = "streamed over " + std::to_string(stream_.tick_count) +
                 " tick(s): " + std::to_string(count) + " " + unit;
    c.scope = "collected in increments of " +
              std::to_string(config.tick_interval.count()) + " ms";
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    result.source_results.push_back(std::move(c));
  };

  if (config.frames) {
    summarise("android.capture.frames", "Frame timing", "dumpsys gfxinfo",
              stream_.frames_ok, out.frames.size(), "frame record(s)");
  }
  if (config.cpu_samples) {
    auto c = make_source("android.capture.cpu_samples", "Sampled CPU stacks",
                         stream_.cpu_ok
                             ? (out.cpu_samples.empty()
                                    ? model::CapabilityStatus::kLimited
                                    : model::CapabilityStatus::kAvailable)
                             : model::CapabilityStatus::kPermissionDenied,
                         "simpleperf");
    c.evidence = "streamed over " + std::to_string(stream_.tick_count) +
                 " tick(s): " + std::to_string(out.cpu_samples.size()) +
                 " sample(s)";
    if (!stream_.cpu_ok) {
      c.evidence += "; last error: " + stream_.cpu_error;
      c.recovery_action =
          "install a debuggable build of the target, or add <profileable "
          "android:shell=\"true\"/> to its manifest";
    }
    c.limitations.push_back(
        "sampled on a background thread in windows of " +
        std::to_string(config.cpu_window.count() / 1000) +
        "s, not on the tick; the intervals between windows are recorded as "
        "coverage gaps rather than as measured idle time");
    {
      std::lock_guard<std::mutex> lock(stream_.cpu_mutex);
      c.limitations.push_back(
          std::to_string(stream_.cpu_windows_done) +
          " sampling window(s) completed, costing " +
          std::to_string(stream_.cpu_busy.count()) +
          " ms including symbolisation");
    }
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    result.source_results.push_back(std::move(c));
  }
  if (config.memory) {
    std::size_t points = 0;
    for (const auto& c : out.counters) points += c.points.size();
    summarise("android.capture.memory", "Process memory counters",
              "dumpsys meminfo", stream_.memory_ok, points, "counter point(s)");
  }

  // The collector's own cost over the session, so a reader can weigh the live
  // numbers against what watching them cost (spec section 9 rule 10).
  {
    auto c = make_source("android.capture.streaming_overhead",
                         "Collector overhead while streaming",
                         model::CapabilityStatus::kAvailable, "mpi collector");
    c.evidence = std::to_string(stream_.tick_count) + " tick(s) costing " +
                 std::to_string(stream_.total_tick_cost.count()) +
                 " ms of device interaction in total";
    c.limitations.push_back(
        "each tick spawns processes on the device; a shorter tick interval "
        "means fresher numbers and more overhead");
    c.limitations.push_back(
        "this is the collector's cost, not the app's, and must not be "
        "subtracted from the app's own measurements");
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    result.source_results.push_back(std::move(c));
  }

  record_failed_source_coverage(out, result.source_results);

  if (config.cancel.cancelled()) {
    out.partial = true;
    out.partial_reasons.push_back("capture stopped by the operator");
  }
  // Counters and a measured launch are evidence too. Deciding this from
  // frames and CPU samples alone threw away a session that held a real
  // startup measurement and five memory families.
  if (!out.counters.empty() || !out.markers.empty()) result.any_data = true;

  if (!result.any_data) {
    out.partial = true;
    out.partial_reasons.push_back("no source produced data");
    result.error =
        "capture completed but no source produced data; see the per-source "
        "results";
  }
  return result;
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

  // ---- scheduling and I/O -------------------------------------------------
  // Opt-in, because atrace traces the whole device. It runs for the capture's
  // own duration, so it describes the same window as everything else.
  if (config.scheduling) {
    auto c = make_source("android.capture.scheduling",
                         "Thread scheduling and I/O waits",
                         model::CapabilityStatus::kUnknown, "atrace");
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    const std::int32_t pid =
        processes.empty() ? 0 : (primary != nullptr ? primary->pid
                                                    : processes.front().pid);
    if (pid <= 0) {
      c.status = model::CapabilityStatus::kUnknown;
      c.evidence = "no pid was resolved, so a system-wide trace could not be "
                   "attributed to the app";
    } else if (collect_scheduling(device, config, pid, out, c) > 0) {
      result.any_data = true;
    }
    result.source_results.push_back(std::move(c));
  }

  // ---- heap dump ----------------------------------------------------------
  // Last, and opt-in. `am dumpheap` pauses the app while the runtime walks
  // the whole heap, so doing it earlier would put that pause inside the
  // window every other source is measuring.
  if (config.heap_dump) {
    auto c = make_source("android.capture.heap_dump", "Heap dump",
                         model::CapabilityStatus::kUnknown, "am dumpheap");
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : model::TestedState::kVerifiedOnSimulatorOrEmulator;
    const std::int32_t heap_pid =
        processes.empty() ? 0 : (primary != nullptr ? primary->pid
                                                    : processes.front().pid);
    if (heap_pid <= 0) {
      c.status = model::CapabilityStatus::kUnknown;
      c.evidence = "no pid was resolved, so no process could be dumped";
    } else {
      const auto path = collect_heap_dump(device, config, heap_pid, c);
      if (!path.empty()) {
        result.artifacts.emplace_back("heap.hprof", path);
        // A heap dump is not evidence about the capture window, so it does
        // not make an otherwise empty capture count as having data.
      }
    }
    result.source_results.push_back(std::move(c));
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
  // A launch this collector performed is evidence with a real device
  // timestamp, and a capture whose only evidence is the startup still has a
  // window.
  for (const auto& m : out.markers) {
    extend(m.timestamp_ns, m.timestamp_ns + m.duration_ns.value_or(0));
  }

  // The memory reading has no timestamp of its own, so it is placed by
  // reading the device clock right after it -- not pinned to the window's end,
  // which is zero when nothing else was collected and would put the reading
  // before the epoch.
  if (!out.counters.empty()) {
    model::TimeNs at = 0;
    const auto up = proc::run(
        shell_argv(device.device_id, {"cat", "/proc/uptime"}), po);
    double seconds = 0.0;
    if (up.ok() && parse_leading_double(up.out, seconds) && seconds > 0.0) {
      at = static_cast<model::TimeNs>(seconds * 1'000'000'000.0);
      extend(at, at);
      // Kept as the capture's clock anchor as well, so a batch capture can
      // still relate another producer's clock to the device's.
      stream_.boot_base_ns = at;
      stream_.host_base = std::chrono::steady_clock::now();
      stream_.clock_id = "android.boottime.ns";
      stream_.clock_anchored = true;
    } else if (have_window) {
      at = hi;
    }
    if (at > 0) {
      for (auto& series : out.counters) {
        for (auto& pt : series.points) pt.first = at;
      }
    } else {
      // No clock could be read: the values are real but unplaceable, so they
      // are dropped rather than stamped with a time they were not taken at.
      out.counters.clear();
      out.ingestion_warnings.push_back(
          "memory counters were discarded: the device clock could not be read, "
          "so the readings could not be placed on the capture timeline");
    }
  }

  if (have_window) {
    out.window_start_ns = lo;
    out.window_end_ns = hi;
  }

  // A batch capture reads each source once, so there is no tick cadence to
  // check -- but the rows still have to exist, or a source that ran and found
  // nothing is indistinguishable from one that never ran.
  record_tick_source_coverage(out, config, /*frame_gaps=*/{},
                              /*check_cadence=*/false);
  record_failed_source_coverage(out, result.source_results);

  if (config.cancel.cancelled()) {
    out.partial = true;
    out.partial_reasons.push_back("capture cancelled by the operator");
  }
  // Counters and a measured launch are evidence too. Deciding this from
  // frames and CPU samples alone discarded a session that held a real startup
  // measurement and five memory families.
  if (!out.counters.empty() || !out.markers.empty()) result.any_data = true;

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
