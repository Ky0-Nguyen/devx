// Attaching to a React Native app's own inspector, through Metro.
//
// A debug build opens this connection by itself -- it is how the "j to open
// debugger" flow works -- so nothing is added to the app, which is the whole
// requirement. Metro lists the connected targets over HTTP and proxies a
// Chrome DevTools Protocol session over a WebSocket.
//
// Kept apart from core/observe/inspect.hpp, which holds the model and the
// assembler. This file is the part that talks to something, and it is the
// part a test cannot exercise without a running app.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/observe/inspect.hpp"
#include "core/util/cancel.hpp"

namespace mpi::rn {

/// One entry from Metro's `/json/list`.
struct InspectorTarget {
  std::string app_id;
  std::string title;
  std::string description;
  std::string device_name;
  std::string websocket_path;   // the path only; the host is always loopback
};

struct TargetList {
  bool metro_reachable = false;
  std::vector<InspectorTarget> targets;
  std::string error;
  /// Set when Metro answered but listed nothing. The distinction matters more
  /// than usual here: no Metro is a tooling problem, while Metro with no
  /// targets means no debug build is connected -- and a release build never
  /// will be.
  bool metro_answered_empty = false;
};

/// Asks Metro what is attached. Loopback only.
TargetList list_targets(std::uint16_t metro_port = 8081);

/// Picks the target to attach to.
///
/// Prefers a full runtime connection over the auxiliary pages Metro also
/// lists, then an exact app id match. Pure, so the preference is testable
/// without Metro.
const InspectorTarget* choose_target(const std::vector<InspectorTarget>& targets,
                                     const std::string& wanted_app_id);

struct InspectOptions {
  std::uint16_t metro_port = 8081;
  std::string app_id;              // empty: whatever is attached
  int seconds = 15;
  /// Read the Redux store's slice names and, if asked, its contents.
  bool read_redux_state = false;
  /// Include the state values, not just the slice names. Off by default: an
  /// app's store holds tokens and personal data, and this report can be
  /// exported.
  bool include_state_values = false;

  /// Take a picture of the screen at the start and end of the window.
  ///
  /// Two rather than one, because one image cannot say whether the app moved.
  /// Both are labelled with when they were taken; neither is offered as the
  /// moment of anything.
  bool screenshots = false;
  /// Which device to photograph, and how. The inspector session itself knows
  /// only the app -- Metro does not say which adb serial the device has --
  /// so this has to be supplied.
  std::string screenshot_device_id;
  model::Platform screenshot_platform = model::Platform::kAndroid;
  std::string screenshot_dir;
};

/// Attaches, listens, and returns what was observed.
///
/// Never throws and never returns a report that claims more than it saw: when
/// nothing can be attached to, the report carries a source marked unavailable
/// with the reason, which is a different answer from an idle app.
observe::InspectReport run(const InspectOptions& options,
                           const CancellationToken* cancel = nullptr);

/// The expression used to locate a Redux store by walking React's own
/// devtools hook.
///
/// Exposed because it is the most surprising part of this feature and the
/// part most likely to break when React changes: a test can at least pin its
/// shape, and a reader can see exactly what is evaluated inside their app.
std::string redux_probe_expression(bool include_values);

}  // namespace mpi::rn
