# DevX v0.1.0

A Mac app and a `mpi` CLI for profiling React Native apps on iOS and Android,
with **nothing added to the app you are profiling**.

## Install

Download `DevX.dmg`, open it, drag DevX to Applications.

**Gatekeeper will refuse it on first launch.** This build is signed ad-hoc,
not notarized, so macOS says the app "cannot be opened because the developer
cannot be verified" — or, if you copied it in an unusual way, that it is
"damaged". It is neither. Right-click DevX in Applications and choose **Open**,
once; after that it launches normally. If you would rather not do that, build
from source: `cmake -B build -G Ninja && cmake --build build --target devx_install`.

- **Apple Silicon only** (arm64). No Intel build.
- macOS 14 or newer.
- `sha256` of `DevX.dmg`: `5560edd4b94c5e45893c0d5d9fb3a48475894d070e110a12917f5407eb28979a`

## What it does

**Profiles without a dependency.** No SDK to import, no rebuild. Android goes
through adb; iOS simulators through simctl and `/usr/bin/sample`; physical iOS
devices through devicectl and xctrace.

**Reads a running app's API calls, console output and Redux store** through the
inspector a React Native debug build already runs — the Reactotron questions,
without Reactotron installed. Network exchanges with headers and bodies, console
lines, and Redux state changes with path-level diffs.

**Says what it did not measure.** A blank column means NOT MEASURED, never zero.
Every capability is reported as available, limited, unsupported, permission-denied
or unknown, and a provider that failed is never presented as a quiet device. This
is the design constraint the whole tool is built around: a profiler that guesses
is worse than no profiler.

**Vietnamese and English.** Switch with the `lang` control at the bottom of the
sidebar. Technical terms stay in English on purpose.

## Known limits

Read `docs/known-limitations.md` before trusting a number. The short version:

- Attaching a debugger changes what the runtime does, so an `inspect` report is
  explicitly **not** a performance measurement.
- Network coverage is the JavaScript side only. A WebView's requests — which is
  most SSO and payment flows — and a native module's do not appear.
- Redux action names need `--redux-actions`, which wraps `dispatch` in the
  running app for the duration. Even then, a thunk's injected dispatch bypasses
  the wrapper; such a change is reported as having no action named rather than
  being attributed to the wrong one.
- The unified log store needs Full Disk Access. Without it xctrace reports
  "the log archive is corrupt", which is not what is wrong.
- No physical-iOS screenshot route exists; it is reported unavailable rather
  than attempted.

## Verification

23 test binaries, 404 Swift checks, and a smoke test covering the CLI surface.
Fixtures are real recorded captures, not constructed ones — one Cognito app
client id is redacted from the CDP fixture, noted in that file's own header.
