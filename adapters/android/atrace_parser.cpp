#include "adapters/android/atrace_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace mpi::android {
namespace {

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::int64_t to_i64(const std::string& s, std::int64_t fallback = 0) {
  const std::string t = trim(s);
  if (t.empty()) return fallback;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(t.c_str(), &end, 10);
  if (errno != 0 || end == t.c_str()) return fallback;
  return static_cast<std::int64_t>(v);
}

// A field written as `key=value`, up to the next space. Returns false when the
// key is absent, which is different from a key whose value is empty.
bool field(const std::string& line, const std::string& key, std::string& out) {
  const auto at = line.find(key + "=");
  if (at == std::string::npos) return false;
  const auto start = at + key.size() + 1;
  const auto end = line.find(' ', start);
  out = line.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
  return true;
}

// A field whose value may contain spaces, which thread names routinely do:
// `next_comm=Firebase Backgr next_pid=3454`. Reading to the next space would
// truncate it to "Firebase", so the value runs to the next ` word=` instead.
bool field_with_spaces(const std::string& line, const std::string& key,
                       std::string& out) {
  const auto at = line.find(key + "=");
  if (at == std::string::npos) return false;
  const auto start = at + key.size() + 1;
  std::size_t end = std::string::npos;
  for (std::size_t i = start; i + 1 < line.size(); ++i) {
    if (line[i] != ' ') continue;
    // A space followed by `word=` starts the next field.
    const auto eq = line.find('=', i + 1);
    if (eq == std::string::npos) break;
    bool word = eq > i + 1;
    for (std::size_t j = i + 1; j < eq && word; ++j) {
      const char c = line[j];
      if (c == ' ') word = false;
    }
    if (word) {
      end = i;
      break;
    }
  }
  out = line.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
  out = trim(out);
  return true;
}

// `17919.213966` -> nanoseconds. Parsed by hand rather than through a double,
// so a 5-hour uptime does not lose microseconds to rounding.
model::TimeNs seconds_to_ns(const std::string& text) {
  const auto dot = text.find('.');
  if (dot == std::string::npos) return to_i64(text) * 1000000000;
  const std::int64_t whole = to_i64(text.substr(0, dot));
  std::string frac = text.substr(dot + 1);
  if (frac.size() > 9) frac.resize(9);
  while (frac.size() < 9) frac.push_back('0');
  return whole * 1000000000 + to_i64(frac);
}

}  // namespace

const char* to_string(ThreadState s) {
  switch (s) {
    case ThreadState::kRunning: return "runnable";
    case ThreadState::kSleeping: return "sleeping";
    case ThreadState::kUninterruptible: return "uninterruptible";
    case ThreadState::kIdleKernel: return "idle_kernel";
    case ThreadState::kDead: return "dead";
    case ThreadState::kOther: return "other";
  }
  return "other";
}

ThreadState thread_state_from_ftrace(const std::string& token) {
  if (token.empty()) return ThreadState::kOther;
  // The first character is the state; `+` means preempted while runnable and
  // `|` separates combined flags, neither of which changes the state itself.
  switch (token[0]) {
    case 'R': return ThreadState::kRunning;
    case 'S': return ThreadState::kSleeping;
    case 'D': return ThreadState::kUninterruptible;
    case 'I': return ThreadState::kIdleKernel;
    case 'X':
    case 'Z': return ThreadState::kDead;
    default: return ThreadState::kOther;
  }
}

AtraceTrace parse_atrace(const std::string& text) {
  AtraceTrace out;
  std::istringstream in(text);
  std::string line;

  while (std::getline(in, line)) {
    if (line.empty()) continue;

    if (line[0] == '#') {
      // The one header line that matters: how much the kernel kept versus how
      // much it wrote. A difference is dropped events.
      const auto at = line.find("entries-in-buffer/entries-written:");
      if (at == std::string::npos) continue;
      const auto rest = trim(line.substr(
          at + std::string("entries-in-buffer/entries-written:").size()));
      const auto slash = rest.find('/');
      if (slash == std::string::npos) continue;
      out.entries_in_buffer = to_i64(rest.substr(0, slash));
      out.entries_written = to_i64(rest.substr(slash + 1));
      out.header_seen = true;
      if (out.entries_written > out.entries_in_buffer) {
        out.dropped_events = out.entries_written - out.entries_in_buffer;
        out.warnings.push_back(
            "the kernel buffer dropped " + std::to_string(out.dropped_events) +
            " event(s): those intervals are gaps in this trace, not quiet "
            "periods on the device. Raise the buffer size or shorten the "
            "capture");
      }
      continue;
    }

    // atrace's own preamble. Recognised rather than counted as unparsed, so
    // `lines_unrecognised` keeps meaning "a trace line I could not read".
    if (line.rfind("capturing trace", 0) == 0 || line == "TRACE:" ||
        line.rfind("TRACE:", 0) == 0) {
      continue;
    }

    ++out.lines_read;

    // ` TASK-TID  ( TGID) [CPU] flags TIMESTAMP: event: fields`
    //
    // The task name can contain spaces and dashes, so the tid is taken from
    // the last '-' before the '(' rather than by splitting on whitespace.
    const auto paren = line.find('(');
    const auto bracket = line.find('[', paren == std::string::npos ? 0 : paren);
    const auto colon_after = line.find(": ", bracket == std::string::npos
                                                ? 0
                                                : bracket);
    if (paren == std::string::npos || bracket == std::string::npos ||
        colon_after == std::string::npos) {
      ++out.lines_unrecognised;
      continue;
    }

    const std::string task_field = trim(line.substr(0, paren));
    const auto dash = task_field.rfind('-');
    if (dash == std::string::npos) {
      ++out.lines_unrecognised;
      continue;
    }
    const std::string emitter_comm = task_field.substr(0, dash);
    const auto emitter_tid = static_cast<std::int32_t>(
        to_i64(task_field.substr(dash + 1), -1));

    const std::string tgid_field =
        trim(line.substr(paren + 1, line.find(')', paren) - paren - 1));
    std::optional<std::int32_t> tgid;
    if (tgid_field.find('-') == std::string::npos && !tgid_field.empty()) {
      tgid = static_cast<std::int32_t>(to_i64(tgid_field, 0));
    }

    const auto cpu_end = line.find(']', bracket);
    const int cpu = static_cast<int>(
        to_i64(line.substr(bracket + 1, cpu_end - bracket - 1), 0));

    // The timestamp sits just before the ": " that introduces the event.
    const auto ts_start = line.rfind(' ', colon_after);
    if (ts_start == std::string::npos) {
      ++out.lines_unrecognised;
      continue;
    }
    const model::TimeNs ts = seconds_to_ns(
        line.substr(ts_start + 1, colon_after - ts_start - 1));
    if (ts <= 0) {
      ++out.lines_unrecognised;
      continue;
    }
    if (out.first_ns == 0 || ts < out.first_ns) out.first_ns = ts;
    out.last_ns = std::max(out.last_ns, ts);

    // `event_name: payload`
    const std::string rest = line.substr(colon_after + 2);
    const auto name_end = rest.find(':');
    if (name_end == std::string::npos) {
      ++out.lines_unrecognised;
      continue;
    }
    const std::string event = trim(rest.substr(0, name_end));
    const std::string payload = trim(rest.substr(name_end + 1));

    if (event == "sched_switch") {
      SchedSwitch sw;
      sw.timestamp_ns = ts;
      sw.cpu = cpu;
      sw.tgid = tgid;
      std::string value;
      if (field_with_spaces(payload, "prev_comm", value)) sw.prev_comm = value;
      if (field(payload, "prev_pid", value)) {
        sw.prev_tid = static_cast<std::int32_t>(to_i64(value, 0));
      }
      if (field(payload, "prev_state", value)) {
        sw.prev_state_raw = value;
        sw.prev_state = thread_state_from_ftrace(value);
      }
      if (field_with_spaces(payload, "next_comm", value)) sw.next_comm = value;
      if (field(payload, "next_pid", value)) {
        sw.next_tid = static_cast<std::int32_t>(to_i64(value, 0));
      }
      out.switches.push_back(std::move(sw));
      continue;
    }

    if (event == "sched_blocked_reason") {
      BlockedReason br;
      br.timestamp_ns = ts;
      br.tgid = tgid;
      std::string value;
      if (field(payload, "pid", value)) {
        br.tid = static_cast<std::int32_t>(to_i64(value, 0));
      }
      if (field(payload, "iowait", value)) br.iowait = to_i64(value, 0) != 0;
      if (field(payload, "caller", value)) br.caller = value;
      out.blocked.push_back(std::move(br));
      continue;
    }

    if (event == "sched_waking" || event == "sched_wakeup") {
      // Only `sched_waking` identifies the waker: it is emitted *by* the
      // waking thread. `sched_wakeup` is emitted on the target's CPU, so its
      // emitter is not the waker and it is skipped rather than
      // misattributed.
      if (event == "sched_wakeup") continue;
      SchedWaking w;
      w.timestamp_ns = ts;
      w.waker_tid = emitter_tid;
      w.waker_comm = emitter_comm;
      std::string value;
      if (field(payload, "pid", value)) {
        w.target_tid = static_cast<std::int32_t>(to_i64(value, 0));
      }
      if (field_with_spaces(payload, "comm", value)) w.target_comm = value;
      out.wakings.push_back(std::move(w));
      continue;
    }

    if (event == "tracing_mark_write") {
      // `trace_event_clock_sync: parent_ts=...` / `realtime_ts=...`
      if (payload.rfind("trace_event_clock_sync", 0) == 0) {
        std::string value;
        if (field(payload, "parent_ts", value)) {
          out.clock_sync.parent_ns = seconds_to_ns(value);
          out.clock_sync.have_parent = true;
        }
        if (field(payload, "realtime_ts", value)) {
          // Milliseconds since the epoch.
          out.clock_sync.realtime_ns = to_i64(value) * 1000000;
          out.clock_sync.have_realtime = true;
        }
        continue;
      }
      // `B|pid|name`, `E|pid`, and the counter form `C|pid|name|value`.
      if (payload.size() < 2 || payload[1] != '|') {
        ++out.lines_unrecognised;
        continue;
      }
      const char kind = payload[0];
      if (kind != 'B' && kind != 'E') continue;  // counters are not slices
      Slice slice;
      slice.timestamp_ns = ts;
      slice.begin = kind == 'B';
      slice.tid = emitter_tid;
      const auto second_bar = payload.find('|', 2);
      slice.pid = static_cast<std::int32_t>(to_i64(
          payload.substr(2, second_bar == std::string::npos
                                ? std::string::npos
                                : second_bar - 2),
          0));
      if (slice.begin && second_bar != std::string::npos) {
        slice.name = trim(payload.substr(second_bar + 1));
      }
      out.slices.push_back(std::move(slice));
      continue;
    }

    // Everything else in the file -- cpu_frequency, cgroup accounting, task
    // renames -- is counted as read but not interpreted.
  }

  if (!out.header_seen) {
    out.warnings.push_back(
        "the ftrace header was missing, so how many events the kernel dropped "
        "is unknown; absence of evidence in this trace cannot be relied on");
  }
  return out;
}

}  // namespace mpi::android
