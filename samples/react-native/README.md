# Sample: an instrumented React Native app

A two-screen app with the SDK wired in. Its purpose is to be copied: the
interesting file is [`instrumentation.js`](instrumentation.js), which owns the
SDK instance and exposes the handful of calls an app actually makes. `App.js`
shows those calls in place.

## What has been run, and what has not

This matters more than the code, so it is first.

| Part | State |
|---|---|
| `instrumentation.js` | **Executed**, against a live `mpi sdk-bridge`, by `verify.mjs`: 23 checks, all passing |
| The marker contract, handshake, transport | **Verified** — the host's own summary is asserted, including that both cancelled navigations arrived as cancels |
| `App.js` | **Not built.** It needs React Native, npm, Gradle and Xcode; none of that runs in this repository |
| On a device or simulator | **Never run.** No physical device has been reached on either platform |

So: the module your app imports is tested. The React components around it are
illustrative, and if they contain a typo, nothing here would have caught it.

## Run the verification

```bash
cmake --build build --target mpi
node samples/react-native/verify.mjs
```

It starts its own bridge, drives the same functions the screens call, stops
the bridge and checks the summary the host prints. No device, no npm, no
network.

## Copy it into a real app

1. Copy `sdk/react-native/mpi-sdk.js` and this directory's
   `instrumentation.js` into the app. Both are plain ES modules with **no
   dependencies and no build step** — adjust the import path in
   `instrumentation.js` to wherever `mpi-sdk.js` landed.
2. Start a capture and let the device reach the host:

   ```bash
   mpi record --device emulator-5554 --app io.example.app --sdk --duration-s 20
   # SDK endpoint http://127.0.0.1:60773 token e5c7675b...
   #   let the device reach it: adb reverse tcp:60773 tcp:60773
   ```

   The transport is 127.0.0.1 only and the token is mandatory. There is no
   wireless path, on purpose.
3. Pass the endpoint and token to `startProfiling`. `App.js` reads them from
   the environment (`MPI_ENDPOINT`, `MPI_TOKEN`), because **a token in source
   is a token in everyone's build** — it grants write access to the capture.

## What the app must not become

Three properties this sample is built to demonstrate, each because the
opposite is easy to write by accident:

- **The app keeps working with no profiler attached.** `startProfiling({})`
  returns false and every later call is inert. The first checks in
  `verify.mjs` are this case, because it is the one every build that nobody is
  profiling will be in.
- **The instrumentation lives in one file.** Nothing in `App.js` talks to the
  SDK directly, so deleting the import is the whole of removing it.
- **A cancelled navigation is its own marker.** React Navigation has no
  explicit cancel, so `onNavigationState` reports a route that leaves the
  state without becoming active as cancelled. "Cancelled" and "never
  finished" are different facts, and a screen the user backed out of is not a
  screen that took forever to load.

## What the SDK cannot tell you

- **Durations are on the app's own clock.** The host will not compare them
  against device measurements without a measured mapping between the two,
  because an assumed offset would line up two unrelated timelines and make
  the result look like evidence.
- **`timedFetch` measures the app's view of a request** — DNS, the TLS
  handshake, a cold radio, retries, and time queued behind other requests in
  the app are all inside it. DET-11 lists every one of those as missing
  evidence on every finding and makes no claim about the server.
- **Expo Go cannot load a custom native module.** This SDK needs none, so it
  works there — but a *native* SDK would require a development build, and the
  spec calls that out (section 11). `sdk/ios` and `sdk/android` are
  deliberately empty: see `docs/known-limitations.md` section 6.
