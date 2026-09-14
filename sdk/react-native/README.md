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
```

Drives this client over real HTTP into the real C++ ingest through
`mpi sdk-bridge`. A mock host would not exercise the two behaviours that only
exist as a pair — sequence gaps and backpressure — so there is no mock.
