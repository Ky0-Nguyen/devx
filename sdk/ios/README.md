# `sdk/ios` — deliberately empty

There is no native iOS SDK in this build, and this file exists so that an
empty directory does not read as unfinished work.

**Why there is none.** The specification calls the app SDK *optional*
(sections 2.5, 4.2) and defines its job as reporting what no device provider
can see: which screen mounted, which navigation was cancelled, which
interaction the user started, and what build the bundle came from. For a React
Native app — the app class this tool is built around — every one of those
facts lives in JavaScript, and
[`sdk/react-native`](../react-native/README.md) reports them with no native
code, no dependencies and no build step.

**What a native SDK would add**, if the target app were not React Native:

- Swift/Objective-C lifecycle markers (`viewDidAppear`, `viewWillDisappear`)
  for a UIKit or SwiftUI app, which have no JavaScript equivalent.
- Build facts read from the app's own bundle rather than passed in by the
  JavaScript layer.
- In-process signposts (`os_signpost`) that Instruments can correlate, which
  would need a C ABI boundary as the specification's technology table says.

**What it would not add.** Nothing about the device: an app-hosted SDK is
sandboxed, and the specification is explicit that it must not be used as if
it could enumerate other applications (section 6.3). Everything this tool
learns about the device comes from host-side tooling.

If a native SDK is built, it belongs here, in Swift with C ABI boundaries
where needed.
