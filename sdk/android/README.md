# `sdk/android` — deliberately empty

There is no native Android SDK in this build, and this file exists so that an
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

- Kotlin/Java lifecycle markers (`Activity.onResume`,
  `Fragment.onDestroyView`) for a native app, which have no JavaScript
  equivalent -- and which would let DET-06 check a lifecycle expectation
  against the app's own statement rather than a framework-private field.
- Build facts read from the app's own `BuildConfig` and manifest rather than
  passed in by the JavaScript layer.
- In-process `Trace.beginSection` calls, which would appear in an `atrace`
  capture alongside the framework's own slices -- DET-03 already reads those
  when the app emits them, so this is the one addition that would improve an
  existing detector rather than add a new source.

**What it would not add.** Nothing about the device: an app-hosted SDK is
sandboxed, and the specification is explicit that it must not be used as if
it could enumerate other applications (section 6.3). Everything this tool
learns about the device comes from host-side tooling.

If a native SDK is built, it belongs here, in Kotlin or Java with JNI
where needed.
