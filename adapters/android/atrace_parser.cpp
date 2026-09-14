#include "adapters/android/atrace_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
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


AtraceMapping map_atrace_to_trace(const AtraceTrace& trace,
                                  const std::vector<AppThread>& threads,
                                  std::int32_t pid,
                                  const std::string& process_instance_id,
                                  model::NormalizedTrace& out) {
  AtraceMapping mapping;
  const std::string& process_key = process_instance_id;

  const auto app_thread = [&](std::int32_t tid) -> const AppThread* {
    for (const auto& t : threads) {
      if (t.tid == tid) return &t;
    }
    return nullptr;
  };

  // Thread identity, so a finding can say *which* thread blocked. The main
  // thread is the one whose tid equals the pid, which is the platform's own
  // convention rather than a guess from the name.
  for (const auto& thread : threads) {
    model::ThreadInfo info;
    info.thread_instance_id = "tid=" + std::to_string(thread.tid);
    info.process_instance_id = process_key;
    info.tid = thread.tid;
    info.name = thread.name;
    info.is_main_ui_thread = thread.tid == pid;
    bool known = false;
    for (const auto& existing : out.threads) {
      if (existing.tid == thread.tid) known = true;
    }
    if (!known) out.threads.push_back(std::move(info));
  }

  // Off-CPU intervals. A switch away from a thread starts one; the next
  // switch *to* that thread ends it. An interval still open at the end of the
  // trace has an unknown duration, which stays absent rather than becoming
  // the rest of the window.
  struct Pending {
    model::TimeNs since = 0;
    ThreadState state = ThreadState::kOther;
    std::string raw;
  };
  std::map<std::int32_t, Pending> off_cpu;

  const auto emit = [&](model::EventCategory category, const std::string& name,
                        model::TimeNs at, std::optional<model::TimeNs> duration,
                        std::int32_t tid, json::Value payload) {
    model::Event e;
    e.event_id = "atrace-" + std::to_string(out.events.size());
    e.provider = "atrace";
    e.clock_domain = "android.boottime.ns";
    e.timestamp = at;
    e.duration_ns = duration;
    e.category = category;
    e.name = name;
    e.process_instance_id = process_key;
    e.thread_instance_id = "tid=" + std::to_string(tid);
    e.payload = std::move(payload);
    out.events.push_back(std::move(e));
    ++mapping.events_emitted;
  };

  for (const auto& sw : trace.switches) {
    const auto* leaving = app_thread(sw.prev_tid);
    const auto* arriving = app_thread(sw.next_tid);
    if (leaving == nullptr && arriving == nullptr) {
      ++mapping.foreign_events;
      continue;
    }
    if (leaving != nullptr) {
      off_cpu[sw.prev_tid] = Pending{sw.timestamp_ns, sw.prev_state,
                                     sw.prev_state_raw};
    }
    if (arriving != nullptr) {
      const auto it = off_cpu.find(sw.next_tid);
      if (it == off_cpu.end()) continue;
      const model::TimeNs duration = sw.timestamp_ns - it->second.since;
      json::Value payload = json::Value::object();
      payload.set("state", json::Value::string(to_string(it->second.state)));
      payload.set("kernel_state", json::Value::string(it->second.raw));
      payload.set("cpu", json::Value::number(static_cast<double>(sw.cpu)));
      if (arriving->name.empty() == false) {
        payload.set("thread_name", json::Value::string(arriving->name));
      }
      payload.set("is_main_thread",
                  json::Value::boolean(sw.next_tid == pid));
      emit(model::EventCategory::kSchedule, to_string(it->second.state),
           it->second.since, duration, sw.next_tid, std::move(payload));
      off_cpu.erase(it);
    }
  }

  // Why a thread blocked, which is the only evidence here that a block was
  // I/O. Without `iowait=1` this stays a schedule event and never becomes an
  // I/O claim.
  for (const auto& blocked : trace.blocked) {
    const auto* thread = app_thread(blocked.tid);
    if (thread == nullptr) {
      ++mapping.foreign_events;
      continue;
    }
    json::Value payload = json::Value::object();
    payload.set("iowait", json::Value::boolean(blocked.iowait));
    payload.set("kernel_caller", json::Value::string(blocked.caller));
    payload.set("thread_name", json::Value::string(thread->name));
    payload.set("is_main_thread", json::Value::boolean(blocked.tid == pid));
    emit(blocked.iowait ? model::EventCategory::kIo
                        : model::EventCategory::kSchedule,
         blocked.iowait ? "blocked_on_io" : "blocked",
         blocked.timestamp_ns, std::nullopt, blocked.tid, std::move(payload));
  }

  // Who woke an app thread. A wake by another process is exactly what a
  // contention claim needs, and it is recorded with the waker's identity
  // rather than inferred from timing.
  for (const auto& waking : trace.wakings) {
    const auto* target = app_thread(waking.target_tid);
    if (target == nullptr) continue;
    json::Value payload = json::Value::object();
    payload.set("waker_tid",
                json::Value::number(static_cast<double>(waking.waker_tid)));
    payload.set("waker_name", json::Value::string(waking.waker_comm));
    payload.set("waker_is_this_app",
                json::Value::boolean(app_thread(waking.waker_tid) != nullptr));
    payload.set("is_main_thread",
                json::Value::boolean(waking.target_tid == pid));
    emit(model::EventCategory::kSchedule, "woken",
         waking.timestamp_ns, std::nullopt, waking.target_tid,
         std::move(payload));
  }

  // The app's own slices, which name the work a blocked stretch interrupted.
  // A nameless slice is skipped: it cannot name anything, and inventing a
  // name would be worse than having none.
  std::map<std::int32_t, std::vector<Slice>> open_slices;
  for (const auto& slice : trace.slices) {
    if (app_thread(slice.tid) == nullptr) continue;
    if (slice.begin) {
      if (slice.name.empty()) continue;
      open_slices[slice.tid].push_back(slice);
      continue;
    }
    auto& stack = open_slices[slice.tid];
    if (stack.empty()) continue;
    const auto begin = stack.back();
    stack.pop_back();
    json::Value payload = json::Value::object();
    payload.set("slice", json::Value::string(begin.name));
    payload.set("is_main_thread", json::Value::boolean(slice.tid == pid));
    emit(model::EventCategory::kOther, begin.name, begin.timestamp_ns,
         slice.timestamp_ns - begin.timestamp_ns, slice.tid,
         std::move(payload));
  }

  if (trace.first_ns > 0) {
    if (out.window_start_ns == 0 || trace.first_ns < out.window_start_ns) {
      out.window_start_ns = trace.first_ns;
    }
    out.window_end_ns = std::max(out.window_end_ns, trace.last_ns);
  }

  return mapping;
}

}  // namespace mpi::android
