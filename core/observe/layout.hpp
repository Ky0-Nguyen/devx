// The layout of a running app's screen, read from the app's own view tree.
//
// Two sources, neither of which needs anything added to the app:
//
//   - iOS simulator: the layout probe (adapters/ios/view_probe) is injected at
//     launch and returns every window's UIView tree and view-controller tree.
//     Code runs inside the app to do it, so the report says so.
//   - Android: `adb shell dumpsys activity top`, which prints the top
//     activity's view hierarchy as the platform keeps it. Nothing runs inside
//     the app.
//
// What this answers: how many views each screen holds, how deep they nest,
// how many are hidden or off screen, which navigation stacks hold how many
// screens, and -- for a React Native app -- how many react-native-screens
// screens are mounted against how many are showing.
//
// What it does not answer: how long anything took. A layout snapshot is one
// instant of the tree, read while the app's main thread was paused for the
// read on iOS. No number in it is a performance measurement, and the
// observations it makes are heuristics with their thresholds stated, never
// detector findings.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model/identity.hpp"
#include "core/util/json.hpp"

namespace mpi::observe {

enum class LayoutSource {
  kIosInjectedProbe,     // the probe dylib, inside the app
  kAndroidDumpsys,       // `dumpsys activity top`, outside the app
};
const char* to_string(LayoutSource s);

/// One view, flattened: the tree is kept as parent indices so a deep screen
/// costs no recursion to analyse.
struct LayoutView {
  std::string cls;      // as the runtime names it
  std::string display;  // demangled when the runtime name was mangled
  /// In window coordinates (iOS) or screen pixels (Android).
  double x = 0, y = 0, w = 0, h = 0;
  bool hidden = false;  // this view itself: hidden (iOS), GONE or INVISIBLE (Android)
  double alpha = 1.0;
  std::string controller;  // iOS: the view controller whose root view this is
  std::string controller_id;  // ...and its identity, matching LayoutController::id
  std::string identifier;  // accessibilityIdentifier (iOS) or resource id (Android)
  int parent = -1;
  int depth = 0;           // from the window root, which is depth 0
  int window = 0;
  int children = 0;
  // Derived by finalize():
  bool effectively_hidden = false;  // itself or an ancestor hidden, or alpha 0
  bool offscreen = false;           // does not intersect its window at all
  bool zero_size = false;
};

struct LayoutController {
  std::string id;  // stable within one snapshot; a root view names it
  std::string cls;
  std::string display;
  std::string kind;  // navigation | tab | split | page | content
  bool view_loaded = false;
  bool visible = false;
  bool presented = false;  // presented modally by its parent
  std::vector<std::string> stack;  // a navigation controller's screens, bottom first
  int parent = -1;
  int depth = 0;
  int window = 0;
};

struct LayoutWindow {
  std::string cls;
  bool key = false;
  bool hidden = false;
  double x = 0, y = 0, w = 0, h = 0;
  int root_view = -1;
};

struct LayoutSnapshot {
  LayoutSource source = LayoutSource::kIosInjectedProbe;
  model::Platform platform = model::Platform::kUnknown;
  std::string device_id;
  std::string app_identifier;
  std::int64_t pid = 0;
  std::int64_t taken_at_unix_ms = 0;
  double screen_w = 0, screen_h = 0, scale = 0;
  /// Android: the activity whose window this is.
  std::string activity;
  /// Android: fragments the activity's FragmentManager reports as added.
  std::vector<std::string> fragments;
  std::vector<LayoutWindow> windows;
  std::vector<LayoutView> views;
  std::vector<LayoutController> controllers;
  /// The probe stops describing views at a cap; a cut tree undercounts.
  bool truncated = false;
  std::int64_t max_views = 0;
  std::vector<std::string> warnings;
};

/// Reads the probe's JSON document. Fails on a document that is not one.
std::optional<LayoutSnapshot> parse_ios_probe(const json::Value& doc,
                                              std::string* error);
/// The JSON reader's limits for a probe document: two levels of nesting per
/// view, so the default depth would refuse any real screen.
json::Limits probe_json_limits();

/// Reads `dumpsys activity top` and takes the hierarchy of `package`'s
/// activity. Fails when that package has no activity in the output, which
/// means it is not on screen.
std::optional<LayoutSnapshot> parse_android_dumpsys(const std::string& text,
                                                    const std::string& package,
                                                    std::string* error);

/// Runtime class names that are mangled (Swift's `_Tt...`), for a caller to
/// demangle with the platform tool and hand back through apply_display_names.
std::vector<std::string> mangled_class_names(const LayoutSnapshot& s);
void apply_display_names(LayoutSnapshot& s,
                         const std::map<std::string, std::string>& demangled);

/// Computes the derived fields. The parsers call it; a snapshot built by hand
/// must too.
void finalize(LayoutSnapshot& s);

struct ClassCount {
  std::string name;
  std::int64_t count = 0;
};

struct LayoutScreen {
  std::string name;
  /// What made this a screen: "view_controller" (iOS), "activity" (Android),
  /// or "react_native_screen" (a react-native-screens screen view).
  std::string basis;
  bool on_screen = false;
  int root_view = -1;
  /// Index into LayoutReport::screens of the screen this one sits inside, or
  /// -1. A React Native screen sits inside the controller or activity that
  /// hosts the React root, so its views are counted in both.
  int inside = -1;
  std::int64_t views = 0;
  std::int64_t visible_views = 0;
  std::int64_t hidden_views = 0;
  std::int64_t offscreen_views = 0;
  int depth = 0;           // deepest view below the screen's root
  int absolute_depth = 0;  // the same view's depth from the window root
  /// Views with exactly one child that covers them exactly: candidates to be
  /// flattened away, and on React Native usually a wrapper `View`.
  std::int64_t single_child_wrappers = 0;
  std::vector<ClassCount> top_classes;
};

struct LayoutNavigation {
  std::string container;  // the navigation controller, or the activity
  std::string kind;       // "navigation_controller" | "fragments"
  bool on_screen = false;
  std::vector<std::string> screens;  // bottom of the stack first
};

struct LayoutReactNative {
  bool detected = false;
  /// "fabric", "paper", or "unknown": from which host view classes exist.
  std::string architecture = "unknown";
  std::int64_t host_views = 0;       // React Native's own view classes
  std::int64_t screens_mounted = 0;  // react-native-screens screen views in the tree
  std::int64_t screens_on_screen = 0;
};

struct LayoutObservation {
  std::string code;
  std::string message;
};

struct LayoutThresholds {
  int depth = 30;
  std::int64_t views_per_screen = 1500;
  std::size_t stack_screens = 6;
  double hidden_share = 0.5;
  std::int64_t hidden_share_min_views = 200;
};

struct LayoutReport {
  LayoutSnapshot snapshot;
  std::int64_t views = 0;
  std::int64_t visible_views = 0;
  std::int64_t hidden_views = 0;
  std::int64_t offscreen_views = 0;
  int max_depth = 0;
  std::vector<LayoutScreen> screens;
  std::vector<LayoutNavigation> navigation;
  LayoutReactNative react_native;
  LayoutThresholds thresholds;
  std::vector<LayoutObservation> observations;

  /// With `include_tree`, every view is included, indented by parent index.
  json::Value to_json(bool include_tree) const;
  std::string to_text(bool include_tree) const;
};

LayoutReport analyze_layout(LayoutSnapshot snapshot,
                            const LayoutThresholds& thresholds = {});

}  // namespace mpi::observe
