# ADR-0007 -- DevX is a native SwiftUI app over a C ABI

- Status: accepted
- Date: 2026-09-14
- Supersedes the deferral in ADR-0004
- Spec references: sections 2.2, 5, 13, 14, 15

## Context

ADR-0004 deferred the desktop UI and fixed the seam it would sit on. The UI is
now wanted, and is named **DevX**.

A loopback web UI was built first and worked, but a browser tab is not a
desktop app: no dock icon, no menu bar, and a token in a URL to manage. The
specification already declares macOS on Apple Silicon as the first host
(section 2.2), Xcode is already a prerequisite for the iOS adapter, and Swift
6.3 with SwiftUI is present.

## Decision

**DevX is a native macOS SwiftUI application.** It reaches the C++ core
through a C ABI (`core/capi/mpi_capi.h`) that returns JSON documents.

The C ABI rather than Swift's C++ interop, because it is the boundary spec
section 5 asks for and it makes three properties structural:

- **No C++ exception crosses it.** Every entry point catches everything and
  returns a JSON error document.
- **No function returns NULL.** A failure is still a parseable document, so the
  UI always has something to show.
- **Nothing is reinterpreted at the boundary.** The JSON is exactly what the
  core serialises, so DevX cannot state something the engine would not. The
  honesty rules stay in the model (ADR-0005) and the UI only renders them.

Apple's frameworks are platform frameworks, not redistributed third-party
code, so ADR-0002's empty licence inventory survives.

## Build

`swiftc` driven from CMake, not `xcodebuild`, so `cmake --build` produces
everything and there is no `.xcodeproj` to keep in sync. The target
self-skips when Swift or the macOS SDK is absent, and in the sanitizer build --
a SwiftUI binary cannot link the sanitizer runtime, and spec section 15 keeps
sanitizers to development tests anyway.

The result is a signed `.app` bundle at `build/bin/DevX.app`.

## `devx-serve` is kept, with a narrower role

The loopback HTTP server survives as `devx-serve`, for a host where SwiftUI
cannot run: a Linux or Windows machine driving the Android workflow, or a
session over SSH. It is not the desktop app and the usage text says so.
Its listener binds `127.0.0.1` only and requires a per-start token, because
spec section 14 forbids an unauthenticated network listener.

## Consequences

- DevX is macOS-only. Aligned with the specification's declared first host,
  but a second host platform would need either `devx-serve` or a second UI.
- Two UIs over one API is real maintenance cost. Accepted because they serve
  genuinely different situations, and because both are thin projections of the
  same JSON rather than parallel implementations of the analysis.
- Launch arguments (`--session`, `--tab`, `--sessions-dir`) let the CLI open a
  session in DevX, which is useful on its own and also made the UI testable
  without Accessibility permission.
- Verified against a real Android emulator: device discovery showing the
  emulator alongside two offline iPhones and the simulators, app enumeration,
  preflight, and the issue drill-down with its provenance banners.

## A bug worth recording

`JSONSerialization` returns `NSNumber` for both numbers and booleans, and
`NSNumber(0) as? Bool` *succeeds* as `false`. Matching `Bool` before `NSNumber`
therefore turned every `0` into `false` and every `1` into `true` -- the
measurement context rendered "false counters" where it meant "0 counters".
Only `CFBoolean` is genuinely a boolean, so the bridge asks
`CFGetTypeID(n) == CFBooleanGetTypeID()` instead. `apps/devx-mac/Tests` pins
this.

## Rejected alternatives

- **Qt 6/QML**, the specification's proposal: a 38-dependency install plus an
  LGPL redistribution review, for a worse result than the platform's own
  framework on the platform the specification names first.
- **Swift C++ interop** instead of a C ABI: fewer moving parts on paper, but it
  puts C++ types and their exception behaviour directly in Swift's path, which
  is exactly what spec section 5 asks to avoid.
- **Tauri or Electron**: a Rust or Node toolchain for a UI that is already a
  thin projection of JSON.
