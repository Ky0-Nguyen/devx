# The in-app SDK (React Native)

Reports what no device provider can see: which screen mounted, which
navigation was **cancelled**, which interaction the user started, and what
build the JS bundle came from.

Without it those fields stay empty — and they stay empty rather than being
guessed, because inferring a screen from a function name is forbidden
(spec section 11). A capture with no SDK says so:

```
app.sdk.markers: unsupported -- no app ever handshook with the SDK endpoint,
so this capture has no screen, navigation or interaction evidence
```

## Install

Copy `mpi-sdk.js` into the app. It is plain ES module JavaScript with **no
dependencies and no build step**, so it can be deleted again as easily as it
was added. It needs `fetch`, which React Native has.

## Use

```bash
# The host prints the endpoint and the token, and the adb line for Android.
mpi record --device emulator-5554 --app io.example.app --sdk --duration-s 20

SDK endpoint http://127.0.0.1:60773 token e5c7675b...
  let the device reach it: adb reverse tcp:60773 tcp:60773
```

```js
import { MpiSdk } from './mpi-sdk.js';

const mpi = new MpiSdk({
  endpoint: 'http://127.0.0.1:60773',
  token: 'e5c7675b...',
  app: {
    app_identifier: 'io.example.app',
    app_version: '4.2.0',
    build_configuration: 'Staging',   // your own name for it, not one of three
    js_engine: 'hermes',
    js_bundle_id: BUNDLE_ID,          // whatever identifies this bundle
    ota_update_id: OTA_ID,            // if you ship OTA updates
    js_dev_mode: __DEV__,
  },
});

await mpi.connect();                   // resolves false; never throws

mpi.screenMount('Checkout');
mpi.navigationBegin('Payment');
mpi.navigationCancel('Payment');       // cancelled, not "never finished"
mpi.interactionBegin('tap-pay');
mpi.interactionEnd('tap-pay', 95_000_000);
mpi.reactCommit({ screen: 'Checkout', component: 'CartList', commitCount: 7 });
mpi.networkRequest({ url: res.url, method: 'GET', status: 200, durationNs });

await mpi.disconnect();                // flushes what is buffered
```

To check an integration without recording anything:

```bash
mpi sdk-bridge            # prints the endpoint, then reports what arrived
```

## Inspect a release build: `trackNetwork()`

A release build runs no inspector, so DevX's Inspect tab has nothing to attach
to. `trackNetwork()` wraps the app's own `fetch` and `XMLHttpRequest` and sends
each request to DevX as the Network events a debugger would have received, with
the request and response bodies. DevX shows them exactly as it shows a debug
build's requests: headers, bodies, GraphQL operations and variables, errors
inside a 200, durations, failures, copy as cURL.

```js
import { Platform } from 'react-native';
import { trackNetwork } from './mpi-sdk';

// Only in a build made for inspecting -- never one that goes to the store.
if (process.env.EXPO_PUBLIC_DEVX_INSPECT === '1') {
  trackNetwork({
    endpoint: 'http://127.0.0.1:47081',   // DevX › Inspect › release build · SDK
    token: 'devx-…',                      // shown there; kept between launches
    app: { app_identifier: 'com.example.app', app_version: '9.1', platform: Platform.OS },
    // detail: false   -> method, URL, status and timing only
    // maxBodyBytes    -> per body, 512 KB by default
    // ignore: ['https://analytics.example.com', /\/health$/]
  });
}
```

Then in DevX: **Inspect › release build · SDK › listen**, run the `adb reverse`
line it shows for an Android device, and open the app. From the command line,
`mpi sdk-network --port 47081 --token devx-…` does the same and prints the
report when stopped.

The port and token stay the same between DevX launches, because a release
build has them built in. **new token** in DevX changes it, which means a new
build.

What it does, so nothing surprises the app:

- **It records only while DevX listens.** Until then it asks every five
  seconds whether DevX is there and records nothing. When DevX stops, what was
  waiting to be sent is discarded, not kept for later.
- **Headers and bodies are sent verbatim**, Authorization included: the point
  is to see what the app sent. They go to the endpoint given and nowhere else.
  Text bodies only; a binary request body is left out and a binary response is
  named by its type.
- **One request is one row.** React Native's `fetch` is built on
  `XMLHttpRequest`; the XHR it creates is not recorded a second time, and
  neither are the tracker's own sends to DevX.
- **Every request goes through untouched.** The wrappers never throw into the
  app; a failure inside the tracker loses a record, never a request.
  `stop()` puts `fetch` and `XMLHttpRequest` back.
- **JavaScript only.** A WebView, a native networking module and the image
  loader are not `fetch` or `XMLHttpRequest`, so they do not appear.
- **Times are the app's own**, measured around the call in JavaScript, so they
  include time the JavaScript thread was busy.

## Reaching the host

The endpoint binds `127.0.0.1` only and requires the token on every request.
There is no wireless path, on purpose (spec section 14 forbids an
unauthenticated network listener, and loopback alone is not authentication —
any local process could reach it).

| Target | How the app reaches it |
|---|---|
| Android device or emulator | `adb reverse tcp:<port> tcp:<port>`, printed for you |
| iOS simulator | loopback is already shared with the host |
| iOS device | a USB tunnel |

## What it promises

- **It never throws into the app.** Every call returns, `connect()` resolves
  `false` on failure, and a transport error becomes `status().lastError`. A
  profiler that crashes the app it measures is worse than no profiler.
- **It counts what it drops.** The buffer is bounded (2048 markers by
  default); when it overflows the oldest goes and the count is reported with
  the next batch, so the host records dropped markers rather than treating
  them as events that never happened.
- **A lost batch becomes a gap, not silence.** Every batch carries a sequence
  number. If one never arrives the host records the gap and says the markers
  are missing evidence, not an absence of activity.
- **It stops instead of retrying forever.** After five consecutive failures it
  gives up and says why through `status()`, rather than spending the app's
  main thread on a host that is not there.
- **It obeys backpressure.** When the host returns 429 the client waits the
  interval it was given and keeps the refused markers for the next attempt.
- **Query strings never leave the app.** `networkRequest` redacts them before
  sending, because they carry tokens, ids and search terms and none of that
  belongs in a performance trace (spec J01).

## Marker kinds

`screen_mount`, `screen_unmount`, `navigation_begin`, `navigation_end`,
`navigation_cancel`, `interaction_begin`, `interaction_end`,
`async_span_begin`, `async_span_end`, `react_commit`, `network_request`,
`runtime_reload`.

Anything else is rejected by the host and counted, rather than stored under a
name no detector will ever query.

## Timestamps

Markers are stamped with `performance.now()` where it exists, reported as
`app.performance.now.ns`. Wall time jumps when the device adjusts its clock,
and a jump inside a capture would reorder markers. The host records which
clock the app used and does **not** silently align it to the device clock: an
unmapped clock stays unmapped (spec section 6).

## Tests

```bash
node sdk/react-native/test/e2e.mjs build/bin/mpi
node sdk/react-native/test/network-e2e.mjs build/bin/mpi
```

The second drives `trackNetwork()` against `mpi sdk-network` with a real API
server, including a `fetch` built on `XMLHttpRequest` as React Native's is.

Drives this client over real HTTP into the real C++ ingest through
`mpi sdk-bridge`. A mock host would not exercise the two behaviours that only
exist as a pair — sequence gaps and backpressure — so there is no mock.
