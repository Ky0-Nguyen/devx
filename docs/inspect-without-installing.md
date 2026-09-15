# Reading a running app without installing anything in it

Reactotron, Flipper and every tool like them work the same way: the app
imports a client, the client opens a socket, and the tool on your machine
talks to it. That means a dependency, an import, a rebuild, and a decision
about whether it ships in release.

`mpi inspect` needs none of that. It reads a connection the app already has.

## Why this is possible

A React Native **debug** build runs an inspector and connects itself to Metro.
It is how "press `j` to open React Native DevTools" works. Metro proxies a
Chrome DevTools Protocol session over a WebSocket, and CDP already carries
what a tool like Reactotron asks the app to send it.

Measured against a real app (HutBot debug build, Android emulator) rather than
assumed:

| What | Where it comes from | Result |
|---|---|---|
| HTTP calls | `Network.requestWillBeSent` / `responseReceived` / `loadingFinished` | method, URL, status, bytes, duration |
| console output | `Runtime.consoleAPICalled`, `Log.entryAdded` | level, arguments, source location |
| JS exceptions | `Runtime.exceptionThrown` | message and stack |
| Redux **state** | `Runtime.evaluate`, walking React's own devtools hook | the store's slices, and its values on request |

The Redux part is the surprising one. `__REDUX_DEVTOOLS_EXTENSION__` is not
present in a React Native app and there is no global `store`, but
`__REACT_DEVTOOLS_GLOBAL_HOOK__` is — it is how React DevTools finds anything
— and from it the fiber tree can be walked to a component prop carrying
`getState`, `dispatch` and `subscribe`. That is a Redux store. On the app this
was built against it was found 23 fibers in, with 52 slices.

The walk is **read-only** and bounded to 4000 fibers. Nothing is assigned,
wrapped or dispatched: a profiler that stalls the app it is measuring has
failed at its own job.

## Using it

```bash
mpi inspect --targets                    # what is attachable, without attaching
mpi inspect --app <id> --seconds 15 --redux
mpi inspect --app <id> --device emulator-5554 --screenshot
```

Or the **Inspect** tab in DevX, which shows the same thing and lets the
observation window run while you use the app.

## What it does not reach

Four limits. Each one is the difference between "we saw nothing" and the false
claim "nothing happened", so they are carried in the report's `caveats` array
and printed under **WHAT THIS DOES NOT SHOW** — not left in this document.

**It needs the inspector.** A release build runs none. There is no target to
attach to, which is a missing provider and not a quiet app. `mpi inspect
--targets` says which of the two you are looking at.

**Network coverage is the JavaScript side only.** These events come from React
Native's own fetch/XHR instrumentation. HTTP performed by a native module
through OkHttp, or an image fetched by the platform's loader, never appears.
An empty list does not mean the app made no requests.

**Redux actions are not observable read-only.** The store's state can be read;
the stream of dispatched actions cannot, because nothing broadcasts it. Seeing
actions would mean wrapping `dispatch` inside the running app — modifying it,
which is the thing this feature exists to avoid. So it is not done, and the
gap is reported rather than filled by inferring actions from state diffs.

The one exception is honest about itself: an app using `redux-logger` prints
its actions to the console, and those lines are recognised. They are reported
as `inferred_redux_action_type` with the basis "the app printed it in
redux-logger's format; the store did not report it" — never as an action the
store reported.

**Attaching is not free.** A debugger session changes what the runtime does;
Hermes may deoptimise. Nothing in an inspect report is offered as a
performance measurement, which is also why this is a separate command from
`record` rather than another source inside it.

## Screenshots

`--screenshot` takes a picture of the screen before and after the window
(`adb exec-out screencap -p`, or `xcrun simctl io … screenshot` for a
simulator; a physical iOS device has no command-line route and is reported
unavailable rather than attempted).

Every image carries when it was taken and therefore what it is evidence *of*:

> the screen after collection ended: it shows where the app finished, and is
> not evidence about any earlier moment

This is the rule the module is built around. **A screenshot shows the screen
at the moment it was taken, and nothing else.** It is not the frame that
missed its deadline. A trace analysed tomorrow cannot be photographed today,
and a ten-second capture has one image out of six hundred frames. Two shots
are taken rather than one, because a single image cannot say whether the app
moved.

Output that is not a valid PNG is refused rather than written. `adb shell`
runs through a pty that turns `\n` into `\r\n` and corrupts every PNG it
carries, so the Android path uses `exec-out`; the header check is what would
catch it if that ever regressed.

## Implementation notes

No dependencies, per ADR-0002. That meant writing:

- `core/net/websocket_client.{hpp,cpp}` — enough of RFC 6455 to speak to a
  debugger: client only, loopback only, text frames and continuations, no
  extensions. A negotiated extension is **refused** rather than ignored,
  because one we cannot decode would corrupt frames silently instead of
  failing. Includes SHA-1 solely to verify `Sec-WebSocket-Accept` — not as a
  security primitive, for which it is broken.
- `core/net/loopback_http.{hpp,cpp}` — the one HTTP GET this tool makes, for
  Metro's target list. Chunked encoding is refused rather than half-parsed.
- `core/observe/inspect.{hpp,cpp}` — the model and the assembler, kept free of
  transport so it can be replayed from a recording.
- `adapters/rn/inspector.{hpp,cpp}` — discovery, the session, the Redux probe.

`fixtures/cdp/recorded-hutbot-startup.jsonl` is a **real** recorded session,
not a constructed one. That matters more than usual here: the whole feature
rests on reading a protocol as a real runtime emits it, and a test written
against the shapes the parser expects would pass while the parser silently
dropped everything real. Two things it caught: `Log.entryAdded` timestamps are
milliseconds while `Network` timestamps are seconds — converting both the same
way put console lines in 1970 beside requests in the present — and console
arguments arrive wrapped in terminal colour codes, because React Native
colours its own warnings.
