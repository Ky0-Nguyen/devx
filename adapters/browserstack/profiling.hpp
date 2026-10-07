// BrowserStack's App Profiling, brought into DevX as a session.
//
// DevX cannot measure on BrowserStack's devices -- they give no adb or shell
// -- but BrowserStack measures when a session runs with `appProfiling`, and
// publishes the result: a summary (CPU, memory, frames, ANRs, battery, I/O,
// load times) and, per metric, a link (`*_data`) to its series over time.
// This turns that into a session package, so the Timeline, Issues, Compare
// and `mpi mcp` read a real device on BrowserStack the way they read one on
// the desk.
//
// The measurements are BrowserStack's: every counter's provider says so, the
// device is described as BrowserStack reported it, and the session states it
// was imported, not captured by DevX. The raw document is kept as an
// observation too, so nothing BrowserStack sent is lost to a field this
// reader does not know.
#pragma once

#include <string>
#include <vector>

#include "adapters/browserstack/browserstack.hpp"
#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::browserstack {

struct ProfilingRequest {
  std::string build_id;
  std::string session_id;  // BrowserStack's session (hashed id)
  // As BrowserStack describes the device and app; filled from the session
  // details when empty.
  std::string device;
  std::string os;          // android | ios
  std::string os_version;
  std::string app;
  std::string sessions_dir;
};

struct ProfilingImport {
  bool ok = false;
  std::string error;
  std::string devx_session_id;
  std::string package_dir;
  std::string observation_id;
  int series = 0;                    // counters built from *_data links
  std::vector<std::string> notes;    // what could not be read, and why
  json::Value summary;
  json::Value to_json() const;
};

/// Fetches the profiling of one session and writes it as a DevX session.
ProfilingImport import_profiling(const Credentials& c, const ProfilingRequest& req,
                                 const CancellationToken& cancel);

/// One measured quantity out of a `*_data` reply: the field it came from and
/// its points, in nanoseconds and the unit BrowserStack gave.
struct Series {
  std::string field;
  std::vector<std::pair<std::int64_t, double>> points;
};

/// Every series in one `*_data` reply. BrowserStack serves an array of
/// samples, each with `time_offset_ms` (from the session's start) and one or
/// more values: `{"time_offset_ms": 3053, "cpu_usage": 1.72, "cpu_threads":
/// 39, "batt_usage": null}` -- so one reply can hold several series, and a
/// null is no sample. Also accepted: other time keys ("t", "ts", "timestamp",
/// "time"; epoch or relative, told apart by magnitude and then counted from
/// the first sample), [[t, v], ...] pairs, a {"data": [...]} wrapper, and CSV
/// with a header. Exposed for the tests.
std::vector<Series> parse_series(const json::Value& doc, const std::string& text);

}  // namespace mpi::browserstack
