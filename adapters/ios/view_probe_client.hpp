// The host side of the layout probe (view_probe/devx_view_probe.mm): finding
// it, launching an app on a simulator with it injected, and asking it for a
// snapshot.
//
// The app is relaunched to inject the probe -- `DYLD_INSERT_LIBRARIES` is read
// only when a process starts -- so its current state is lost. That is why it
// is never done implicitly: a snapshot of an app that was not launched with
// the probe is refused with the flag that would do it, rather than the app
// being restarted under someone who was halfway through a flow.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"

namespace mpi::ios {

/// The socket the probe listens on for one app on one simulator. Derived from
/// both, so a later `mpi layout` finds a probe an earlier one loaded without
/// relaunching, and two apps never share one. Short enough for a Unix
/// socket's 104-byte path limit.
std::string probe_socket_path(const std::string& udid,
                              const std::string& bundle_id);

/// The probe dylib: `MPI_VIEW_PROBE` when set, else beside this executable
/// (a build tree), else in `../Resources` (inside DevX.app, which is also
/// where the Homebrew `mpi` link resolves to). `searched` lists where it
/// looked, for the error when it is in none of them.
std::optional<std::string> find_view_probe(std::vector<std::string>* searched);

struct ProbeLaunch {
  bool launched = false;
  std::int64_t pid = 0;
  std::string error;
};

/// `xcrun simctl launch --terminate-running-process` with the probe injected
/// through `SIMCTL_CHILD_*`. A running instance is terminated first.
ProbeLaunch launch_with_probe(const std::string& udid,
                              const std::string& bundle_id,
                              const std::string& probe_path,
                              const std::string& socket_path,
                              std::chrono::milliseconds timeout,
                              const CancellationToken& cancel);

enum class ProbeAnswer {
  kAnswered,
  kNotLoaded,  // nothing listening: the app was not launched with the probe
  kTimedOut,   // listening, but no complete answer in time (main thread busy)
  kFailed,
};
const char* to_string(ProbeAnswer a);

struct ProbeFetch {
  ProbeAnswer answer = ProbeAnswer::kFailed;
  std::string body;
  std::string error;
};

/// Connects and reads one snapshot. With `wait_for_listener` it keeps
/// retrying the connection until the deadline, which is what a just-launched
/// app needs; without it, nothing listening is answered at once.
ProbeFetch fetch_probe_snapshot(const std::string& socket_path,
                                std::chrono::milliseconds timeout,
                                bool wait_for_listener,
                                const CancellationToken& cancel);

/// Swift's mangled runtime names, demangled by `xcrun swift-demangle`. A name
/// it cannot demangle maps to itself; a missing tool yields an empty map.
std::map<std::string, std::string> demangle_swift_names(
    const std::vector<std::string>& names, std::chrono::milliseconds timeout);

}  // namespace mpi::ios
