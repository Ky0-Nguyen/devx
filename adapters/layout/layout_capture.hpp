// Taking one layout snapshot of an app, on whichever platform its device is.
//
// Shared by `mpi layout` and the C ABI DevX uses, so the two cannot drift on
// the rules that matter: a physical iOS device is refused, a system app is
// refused, and an iOS app is relaunched with the probe only when asked.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/model/identity.hpp"
#include "core/observe/layout.hpp"
#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::layout {

struct CaptureOptions {
  /// iOS simulator: restart the app with the probe injected. Its current
  /// state is lost. Ignored on Android, which needs nothing launched.
  bool relaunch = false;
  /// After a relaunch, how long to let the first screen render.
  std::chrono::milliseconds settle{3000};
  std::chrono::milliseconds timeout{15000};
  CancellationToken cancel;
  /// Told what is about to happen, before it does -- "relaunching" is said
  /// before the app is killed, not after.
  std::function<void(const std::string&)> progress;
  /// When set, the report -- every view included -- is saved as an
  /// observation under this sessions directory, where `mpi mcp` reads it.
  std::string save_to;
};

enum class Failure {
  kNone,
  kUnsupported,    // physical iOS device, system app, unknown platform
  kProbeMissing,   // the probe library is not built or not bundled
  kNotLoaded,      // iOS: the app was not launched with the probe
  kNotOnScreen,    // Android: the package has no activity showing
  kCollection,     // a tool failed or answered with something unreadable
  kCancelled,
};
const char* to_string(Failure f);

struct Capture {
  std::optional<observe::LayoutReport> report;
  Failure failure = Failure::kNone;
  std::string error;
  std::vector<std::string> notes;
  std::int64_t launched_pid = 0;
  /// Set when the report was saved: the observation id and its file.
  std::string saved_id;
  std::string saved_path;
  std::string save_error;

  bool ok() const { return report.has_value(); }
  json::Value to_json(bool include_tree) const;
};

Capture capture_layout(const model::DeviceRef& device, const std::string& app,
                       const CaptureOptions& options);

}  // namespace mpi::layout
