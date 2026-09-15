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
#include <memory>
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
  /// Metro's own per-device identity, from the `device=` parameter of the
  /// debugger URL.
  ///
  /// This exists because **device names are not unique**. Two different
  /// simulators of the same model on different runtimes both report
  /// `iPad (A16)` -- verified: UDIDs 7ABCF841… (iOS 18.6) and 1909934C…
  /// (iOS 26.5). Grouping by name would merge two genuinely different devices
  /// into one and pick between them silently, which is the bug that device
  /// selection was added to prevent.
  ///
  /// Several targets share one key when Metro lists a runtime connection plus
  /// its auxiliary pages -- that is the same device and is resolved by
  /// preference.
  std::string device_key;
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

/// The outcome of choosing a target.
///
/// Ambiguity is a distinct answer from "found" and from "not found", because
/// the same app id commonly runs on several devices at once against one Metro
/// -- an Android emulator and an iOS simulator, say -- and picking one of
/// them silently reports the wrong device's traffic under the right app's
/// name. That happened: with the app on an emulator and two iOS targets,
/// asking for it always attached to Android, and there was no way to reach
/// iOS at all.
struct TargetChoice {
  const InspectorTarget* target = nullptr;
  /// Set when several devices offer the app and nothing said which.
  bool ambiguous = false;
  /// The device names to choose between, when ambiguous, or the ones that
  /// were available when a hint matched nothing.
  std::vector<std::string> device_names;
  std::string error;
};

/// Picks the target to attach to.
///
/// `device_hint` is matched case-insensitively as a substring of Metro's
/// `deviceName` **or** of its device key. Metro publishes no adb serial and
/// no simulator UDID, so a name is what an operator has to go on -- but when
/// two devices share a name, the key is how they are told apart, and the
/// listing shows it for exactly those. Empty means "whichever, as long as
/// there is only one".
///
/// Prefers a full runtime connection over the auxiliary pages Metro also
/// lists for the same device. Pure, so the preference and the ambiguity rule
/// are testable without Metro.
TargetChoice choose_target(const std::vector<InspectorTarget>& targets,
                           const std::string& wanted_app_id,
                           const std::string& device_hint = {});

struct InspectOptions {
  std::uint16_t metro_port = 8081;
  std::string app_id;              // empty: whatever is attached
  /// Which device, matched against Metro's `deviceName`. Empty is only valid
  /// when one device offers the app.
  std::string device_hint;
  int seconds = 15;
  /// Read the Redux store's slice names and, if asked, its contents.
  bool read_redux_state = false;
  /// Include the state values, not just the slice names. Off by default: an
  /// app's store holds tokens and personal data, and this report can be
  /// exported.
  bool include_state_values = false;

  /// Capture request and response headers, and response bodies.
  ///
  /// This is what makes a request inspectable rather than merely listed, and
  /// it is off by default because it is where the secrets are: an
  /// `Authorization` header carries a bearer token and a login response
  /// carries whatever the login returned. Verified available with nothing
  /// added to the app -- `Network.getResponseBody` returned a real body over
  /// the inspector socket.
  bool capture_detail = false;

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

/// An observation that stays open, so the data arrives while it happens.
///
/// `run()` above returns when its window closes, which is fine for a scripted
/// capture and wrong for a person: the interesting API calls happen when you
/// tap something, and a report that appears fifteen seconds later cannot be
/// connected to what you just did. This keeps the socket open and hands back
/// whatever has arrived so far.
///
/// The assembler is cumulative, so each poll returns the whole observation to
/// date rather than a delta. That is deliberate: a network exchange is
/// assembled from three separate events, and a caller stitching deltas back
/// together would have to re-implement that -- and would show a request with
/// no status for as long as the response had not arrived yet.
class InspectStream {
 public:
  InspectStream();
  ~InspectStream();
  InspectStream(const InspectStream&) = delete;
  InspectStream& operator=(const InspectStream&) = delete;

  /// Attaches. Returns false and fills `error` when there is nothing to
  /// attach to; the reason distinguishes "no Metro", "no debug build
  /// connected" and "the debugger slot is taken", because they need different
  /// things done about them.
  bool start(const InspectOptions& options, std::string* error);

  /// Reads whatever has arrived, for at most `budget_ms`. Never blocks longer
  /// than that, so a caller can poll from a UI without freezing it.
  void pump(int budget_ms);

  /// The observation so far.
  observe::InspectReport snapshot() const;

  bool running() const { return running_; }
  /// Set once the app closed the connection or the socket failed. The report
  /// says so, because a capture that ended early is partial and partial is
  /// not quiet.
  const std::string& disconnect_reason() const { return disconnect_reason_; }

  void stop();

 private:
  void handle(const json::Value& message);
  void request_pending_bodies();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool running_ = false;
  std::string disconnect_reason_;
};

/// The expression used to locate a Redux store by walking React's own
/// devtools hook.
///
/// Exposed because it is the most surprising part of this feature and the
/// part most likely to break when React changes: a test can at least pin its
/// shape, and a reader can see exactly what is evaluated inside their app.
std::string redux_probe_expression(bool include_values);

}  // namespace mpi::rn
