#include "core/session/live_capture.hpp"

#include <algorithm>

#include "core/ingestion/normalize.hpp"
#include "core/util/time.hpp"

namespace mpi::session {

const char* to_string(LiveState s) {
  switch (s) {
    case LiveState::kIdle: return "idle";
    case LiveState::kStarting: return "starting";
    case LiveState::kRunning: return "running";
    case LiveState::kStopping: return "stopping";
    case LiveState::kFinished: return "finished";
    case LiveState::kFailed: return "failed";
  }
  return "idle";
}

json::Value LiveSnapshot::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  v.set("state", json::Value::string(to_string(state)));
  v.set("session_id", json::Value::string(session_id));
  v.set("error", error.empty() ? json::Value::null() : json::Value::string(error));
  v.set("elapsed_ms", json::Value::integer(elapsed.count()));
  v.set("ticks", json::Value::integer(ticks));
  v.set("collector_cost_ms", json::Value::integer(collector_cost.count()));
  v.set("last_tick_cost_ms", json::Value::integer(last_tick_cost.count()));

  json::Value counts = json::Value::object();
  counts.set("frames", json::Value::integer(frames));
  counts.set("cpu_samples", json::Value::integer(cpu_samples));
  counts.set("counter_points", json::Value::integer(counter_points));
  counts.set("threads", json::Value::integer(threads));
  v.set("counts", std::move(counts));

  v.set("window_start_ns", json::Value::integer(window_start_ns));
  v.set("window_end_ns", json::Value::integer(window_end_ns));
  v.set("window_duration_ns",
        json::Value::integer(window_end_ns - window_start_ns));

  json::Value srcs = json::Value::array();
  for (const auto& c : source_status) srcs.push_back(c.to_json());
  v.set("source_status", std::move(srcs));

  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  v.set("notes", std::move(n));

  json::Value latest = json::Value::array();
  for (const auto& kv : latest_counters) {
    json::Value e = json::Value::object();
    e.set("name", json::Value::string(kv.first));
    e.set("value", json::Value::number(kv.second));
    latest.push_back(std::move(e));
  }
  v.set("latest_counters", std::move(latest));

  v.set("preliminary_analysis", preliminary_analysis.to_json());
  // Not a UI convention: the document itself states that these findings are
  // over a window that is still open (spec sections 2.2 and 13).
  v.set("analysis_is_preliminary", json::Value::boolean(analysis_is_preliminary));
  v.set("analysis_caveat",
        json::Value::string(
            analysis_is_preliminary
                ? "PRELIMINARY: the capture window is still open. A detector "
                  "that has found nothing yet may still fire, and a finding "
                  "may change as more evidence arrives. Nothing here is a "
                  "final result."
                : "final: computed over the closed capture window"));
  return v;
}

LiveSession::LiveSession() = default;

LiveSession::~LiveSession() { stop(); }

bool LiveSession::start(std::shared_ptr<Collector> collector,
                        const model::DeviceRef& device,
                        std::vector<model::ProcessInstance> processes,
                        CaptureConfig config, model::NormalizedTrace seed) {
  stop();

  std::lock_guard<std::mutex> lock(mutex_);
  collector_ = std::move(collector);
  device_ = device;
  processes_ = std::move(processes);
  config_ = std::move(config);
  trace_ = std::move(seed);
  result_ = CaptureResult{};
  snapshot_ = LiveSnapshot{};
  snapshot_.session_id = trace_.session_id;
  snapshot_.state = LiveState::kStarting;
  stop_requested_.store(false);
  cancel_ = CancellationSource();
  config_.cancel = cancel_.token();

  if (!collector_) {
    snapshot_.state = LiveState::kFailed;
    snapshot_.error = "no collector was supplied";
    return false;
  }
  if (!collector_->supports_streaming()) {
    snapshot_.state = LiveState::kFailed;
    snapshot_.error = "collector '" + collector_->id() +
                      "' does not support streaming; use a batch capture";
    return false;
  }

  const auto begun = collector_->begin(device_, processes_, config_, trace_);
  if (!begun.started) {
    snapshot_.state = LiveState::kFailed;
    snapshot_.error = begun.error.empty() ? "collector failed to start"
                                          : begun.error;
    return false;
  }

  started_at_ = std::chrono::steady_clock::now();
  snapshot_.state = LiveState::kRunning;
  thread_ = std::thread([this] { run_loop(); });
  return true;
}

void LiveSession::stop() {
  if (!thread_.joinable()) return;
  stop_requested_.store(true);
  // The collector polls the cancellation token, so a tick already in flight
  // unwinds rather than being abandoned.
  cancel_.cancel();
  thread_.join();
}

bool LiveSession::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.state == LiveState::kRunning ||
         snapshot_.state == LiveState::kStarting;
}

LiveSnapshot LiveSession::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

model::NormalizedTrace LiveSession::take_trace() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::move(trace_);
}

CaptureResult LiveSession::result() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

std::shared_ptr<Collector> LiveSession::collector() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return collector_;
}

void LiveSession::run_loop() {
  auto last_analysis = std::chrono::steady_clock::now() - analysis_interval_;

  while (!stop_requested_.load()) {
    LiveUpdate update;
    {
      // The collector is driven under the lock so the trace is never mutated
      // while a snapshot is being copied out of it.
      std::lock_guard<std::mutex> lock(mutex_);
      if (snapshot_.state != LiveState::kRunning) break;
      update = collector_->tick(device_, processes_, config_, trace_);
    }

    const auto now = std::chrono::steady_clock::now();
    const bool analyse = (now - last_analysis) >= analysis_interval_;
    if (analyse) last_analysis = now;
    refresh_snapshot(&update, analyse);

    if (stop_requested_.load()) break;

    // A tick that took longer than the interval means the device is the
    // bottleneck; the next tick starts immediately rather than falling further
    // behind.
    const auto remaining = config_.tick_interval - update.tick_cost;
    if (remaining.count() > 0) {
      const auto deadline = std::chrono::steady_clock::now() + remaining;
      while (!stop_requested_.load() &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = LiveState::kStopping;
  }

  CaptureResult final_result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stopping is a normal end, not a cancellation of the data: the window is
    // closed and what arrived is kept. The cancellation token was used to
    // unwind the collector, so it is cleared before finishing so `finish` does
    // not read the session as aborted.
    config_.cancel = CancellationToken::none();
    final_result = collector_->finish(device_, processes_, config_, trace_);
    ingest::normalize(trace_);
    result_ = final_result;
  }

  // The final analysis is over a closed window, so it is not preliminary.
  refresh_snapshot(nullptr, /*run_analysis=*/true);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.analysis_is_preliminary = false;
    snapshot_.state = final_result.started && final_result.error.empty()
                          ? LiveState::kFinished
                          : (final_result.any_data ? LiveState::kFinished
                                                   : LiveState::kFailed);
    if (!final_result.error.empty()) snapshot_.error = final_result.error;
    snapshot_.source_status = final_result.source_results;
  }
}

void LiveSession::refresh_snapshot(const LiveUpdate* update, bool run_analysis) {
  // Analysis runs on a copy taken under the lock, then the result is published
  // under the lock again. Holding the lock across the analysis would stall
  // every snapshot read for its duration.
  model::NormalizedTrace working;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at_);
    if (update != nullptr) {
      ++snapshot_.ticks;
      snapshot_.collector_cost += update->tick_cost;
      snapshot_.last_tick_cost = update->tick_cost;
      snapshot_.source_status = update->source_status;
      for (const auto& n : update->notes) {
        if (std::find(snapshot_.notes.begin(), snapshot_.notes.end(), n) ==
            snapshot_.notes.end()) {
          snapshot_.notes.push_back(n);
        }
      }
    }
    snapshot_.frames = static_cast<std::int64_t>(trace_.frames.size());
    snapshot_.cpu_samples = static_cast<std::int64_t>(trace_.cpu_samples.size());
    snapshot_.threads = static_cast<std::int64_t>(trace_.threads.size());
    std::int64_t points = 0;
    snapshot_.latest_counters.clear();
    for (const auto& c : trace_.counters) {
      points += static_cast<std::int64_t>(c.points.size());
      if (!c.points.empty()) {
        snapshot_.latest_counters.emplace_back(c.name, c.points.back().second);
      }
    }
    snapshot_.counter_points = points;
    snapshot_.window_start_ns = trace_.window_start_ns;
    snapshot_.window_end_ns = trace_.window_end_ns;
    if (!run_analysis) return;
    working = trace_;
  }

  // Normalizing the copy rather than the live trace: the live trace keeps
  // arrival order, and normalizing it repeatedly mid-capture would reorder
  // events under the collector's feet.
  ingest::normalize(working);
  symbols::SymbolService symbol_service;
  rules::EngineOptions opts;
  opts.mode = working.requested_mode;
  auto analysis = rules::analyze(working, symbol_service, opts);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool still_open = snapshot_.state == LiveState::kRunning ||
                            snapshot_.state == LiveState::kStarting;
    if (still_open) {
      analysis.data_quality_notes.insert(
          analysis.data_quality_notes.begin(),
          "PRELIMINARY: this analysis ran while the capture window was still "
          "open. A detector that found nothing may still fire, and a finding "
          "may change as more evidence arrives.");
    }
    snapshot_.preliminary_analysis = std::move(analysis);
    snapshot_.analysis_is_preliminary = still_open;
  }
}

}  // namespace mpi::session
