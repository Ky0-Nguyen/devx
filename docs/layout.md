# Layout: how a screen is built

`mpi layout` and DevX's **Layout** tab answer one question about the screen an
app is showing: how is it built? How many views each screen holds, how deep
they nest, how many are hidden or off screen, which navigation stacks hold how
many screens, and, for a React Native app, how many screens are mounted
against how many are showing.

The app is selected by package name or bundle id. **Nothing is added to it.**

```bash
mpi layout --device <udid> --app <bundle-id> --relaunch   # iOS simulator, first time
mpi layout --device <udid> --app <bundle-id>              # later: reads whatever screen it is on
mpi layout --device <serial> --app <package>              # Android emulator or device
mpi layout ... --json --tree                              # every view, for a script
```

Every snapshot is also saved, every view included, as an observation under
`~/.mpi/sessions/observations/`, where `mpi mcp` lets an AI tool list and read
it (`list_observations`, `read_observation`), so a model can work from the
whole tree rather than from a screenshot of the tab. `--no-save` skips it; see
[the MCP server](mcp-server.md#what-is-kept-for-later).

A snapshot describes structure at one instant. **It is never a performance
measurement**, and its observations are heuristics with their thresholds
stated, not detector findings.

## Where the tree comes from

### Android: `dumpsys activity top`

The platform prints the view hierarchy of the top activity of every visible
task. `mpi layout` takes the block for the package asked about. Nothing runs
inside the app, so it works on an emulator and on a physical device, for a
release build as well as a debug one. The app has to be in the foreground: a
package with no activity in the output is reported as not on screen.

Limits of the source, and they show in the report:

- Bounds are relative to the parent, and scroll offsets are not printed, so a
  view inside a scrolled container is placed as if it were not scrolled.
  Off-screen counts below a scroll view are approximate; the report says so
  in a warning.
- The fragment list is what the activity's FragmentManager reports as added.
  It is not a stack, so nothing in it is called the top. androidx's headless
  `ReportFragment` is left out.

### iOS simulator: the layout probe

There is no command-line route to an iOS app's view tree. So on a simulator,
DevX brings its own: a small library, `libdevx_view_probe.dylib`, built from
[`adapters/ios/view_probe/devx_view_probe.mm`](../adapters/ios/view_probe/devx_view_probe.mm)
with the project and shipped inside DevX.app.

How it gets in. An app on the simulator is a process on your Mac, and
`xcrun simctl launch` passes every `SIMCTL_CHILD_*` variable to the app it
starts. `--relaunch` uses `SIMCTL_CHILD_DYLD_INSERT_LIBRARIES` to load the
probe at launch. The app bundle is not touched, nothing is installed on the
simulator, and the library stays on the Mac. Xcode's view debugger works the
same way.

What it does:

- Nothing, unless `DEVX_PROBE_SOCKET` names a socket, which only `mpi` sets.
  It then clears both variables so nothing the app launches inherits them.
- It listens on that Unix socket, owner-only (`0600`), at a path derived from
  the simulator and the bundle id, so a later run finds it without
  relaunching and two apps never share one.
- Each connection gets one JSON document: every window's view tree (class,
  frame in window coordinates, hidden, alpha, accessibility identifier) and
  view-controller tree (kind, visible, a navigation controller's stack,
  presented controllers). A controller and its root view carry the same id,
  so the two trees join exactly. Joining by class name was wrong whenever a
  tab bar held four controllers of one class, and joining through the
  responder chain dropped every SwiftUI screen.
- The tree is read **on the main thread**, because UIKit is not thread-safe.
  The app's UI pauses while the walk runs.
- At most 60,000 views are described. A tree cut there says `truncated`, and
  every count becomes a lower bound.

Swift's mangled class names (`_TtGC7SwiftUI19UIHostingController…`) are
demangled on the Mac with `xcrun swift-demangle`.

What it refuses:

- **A physical iOS device.** Code signing will not load an injected library
  there, and there is no other route.
- **System apps** (`com.apple.*`). The simulator does not load an injected
  library into them, which was checked against Settings.
- **An implicit relaunch.** Loading the probe restarts the app and loses its
  state, so a snapshot of an app not launched with it fails with
  `probe_not_loaded` and the flag that would fix it. In DevX the relaunch
  button asks first.

## What the report contains

| Section | What it says |
|---|---|
| totals | windows, views, visible, hidden (itself or an ancestor hidden, or alpha 0), off screen (outside its window), deepest view |
| screens | iOS: each innermost visible content view controller, so containers (navigation, tab bar) and an outer controller that only hosts another are not screens; UIKit's own windows (keyboard, text effects) are left out. Android: the activity. React Native: one per react-native-screens screen view, saying which screen it sits inside. Per screen: views, visible, hidden, off screen, depth below its root, single-child wrappers, top classes |
| navigation | iOS navigation controllers and their stacks, bottom first; Android's added fragments |
| react_native | host views, the architecture (fabric or paper on iOS, from which host classes exist), react-native-screens screens mounted and showing |
| observations | `deep_screen` (depth > 30), `heavy_screen` (> 1,500 views), `deep_stack` (> 6 screens), `many_mounted_screens` (> 6 hidden), `mostly_not_shown` (> 50% of at least 200 views), `tree_truncated` |

A *single-child wrapper* is a view with exactly one child that covers it
exactly, and no controller or identifier of its own. It is a candidate to be
flattened away; on React Native it is usually a wrapper `View`.

## What was exercised

- iOS: Expo Go on an iPhone 17 simulator (iOS 26.0), relaunched with the
  probe: 3 windows, 87 views, the SwiftUI home screen at the top of its
  navigation stack. Recorded as
  [`fixtures/layout/ios-probe-expo-go-home.real.json`](../fixtures/layout/ios-probe-expo-go-home.real.json).
- Android: Settings on a Pixel 6a emulator (API 33): 174 views, depth 12,
  three added fragments. Recorded as
  [`fixtures/layout/android-dumpsys-activity-top-settings.real.txt`](../fixtures/layout/android-dumpsys-activity-top-settings.real.txt).
- React Native on Android: a React Native CLI 0.87 debug app (new
  architecture) on a Pixel 9 emulator (API 35): 24 views, 17 React Native
  host views (`ReactTextView`, `ReactViewGroup`), detected as React Native.
  The architecture was reported unknown, because nothing in a `dumpsys`
  dump says which renderer drew a view.
- The React Native rules (host views, architecture, screens mounted against
  showing) go by class names, `RCT*` and `RNSScreenView` on iOS,
  `com.facebook.react.*` and `com.swmansion.rnscreens.Screen` on Android, and
  are also tested against a labelled synthetic tree. A navigator that does
  not use react-native-screens mounts no screen view, and then reports none;
  that case, and iOS React Native, are not yet measured on a real app.
