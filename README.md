# Mobile Performance Inspector (`mpi`)

A cross-platform mobile performance profiler with a C++20 analysis core.

Implements **M0**, **M1** and the Android half of **M2** of
`MOBILE_PROFILER_IMPLEMENTATION_SPEC_EN.md` v2.0.

Three pieces:

- **`mpi`** — the headless CLI.
- **`DevX.app`** — the native macOS desktop app (SwiftUI over a C ABI).
- **`devx-serve`** — the same views over loopback HTTP, for a host without
  SwiftUI.

> **Read this first.** Android live capture works and is verified against a
> real emulator. **iOS live capture is not implemented**, and no physical
> device of either platform was reachable during development, so the
> physical-device path is explicitly **unverified**. `mpi record` refuses to
> fabricate a capture. What works, what does not, and what was measured:
> [`docs/known-limitations.md`](docs/known-limitations.md) and
> [`docs/capabilities/tested-capability-matrix.md`](docs/capabilities/tested-capability-matrix.md).

---

## What works today

| | Android | iOS |
|---|---|---|
| Device discovery | ✅ `adb devices -l`, parsed and tested | ✅ `devicectl list devices` + `simctl`, verified against real output |
| Installed-app enumeration | ⚙️ implemented, **not verified** (no device) | ✅ verified on a booted simulator; ⚙️ physical path implemented, not verified |
| Running-process resolution | ⚙️ implemented, **not verified** | ✅ verified on simulator via launchd; ⚙️ physical path not verified |
| Selection by package name / bundle id | ✅ | ✅ |
| Ownership evidence + PID-reuse safety | ✅ | ✅ |
| Capability preflight | ✅ | ✅ |
| **Live capture** | ✅ verified on emulator (frames, CPU, memory) | ❌ collector not wired up |
| **Realtime streaming** (open window, updates as they arrive) | ✅ verified on emulator: 62 ticks, frames + memory per tick, CPU in background windows | ❌ needs the collector first |
| Offline analysis, issues, evidence | ✅ | ✅ |
| JSON / Markdown reports | ✅ | ✅ |
| Benchmark comparison engine | ✅ | ✅ |
| Desktop app (DevX) | ✅ | ✅ |

✅ exercised against real tooling · ⚙️ implemented and unit-tested, awaiting hardware · ❌ not implemented

**A PID is never required.** Targets are selected by Android package name or
iOS bundle identifier, as spec section 1 requires.

---

## Build

Requirements: a C++20 compiler, CMake ≥ 3.24. No third-party libraries
([why](docs/adr/0002-zero-third-party-dependencies.md)), so no network access
is needed to build.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Run the tests (355 C++ cases in 16 binaries, plus 61 Swift cases):

```bash
cd build && ctest --output-on-failure
```

Build with ASan + UBSan — development only, never for benchmark capture
(spec §15):

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMPI_ENABLE_SANITIZERS=ON
cmake --build build-asan && (cd build-asan && ctest --output-on-failure)
```

Artifacts land in `build/bin/`: `mpi`, `DevX.app`, `devx-serve`. Warnings are
errors by default (`-DMPI_ENABLE_WARNINGS_AS_ERRORS=OFF` to relax).

The SwiftUI app needs macOS with Swift and the macOS SDK; its build target
self-skips when either is missing, so the repository still builds elsewhere.

### Platform prerequisites

Only needed for live discovery, not for offline analysis.

- **Android** — Android SDK Platform Tools on `PATH`; USB debugging enabled and
  authorized on the device.
- **iOS** — macOS with Xcode installed and selected (`xcode-select -p`). A
  physical device must be unlocked, trusted, and have Developer Mode enabled;
  Xcode must have prepared its developer disk image (the tool reports
  `ddiServicesAvailable: false` when it has not).

---

## DevX, the desktop app

```bash
open build/bin/DevX.app

# or open a specific session straight from the CLI
open -a "$PWD/build/bin/DevX.app" --args --session=<session-id>

# or start streaming a live capture on launch
open -a "$PWD/build/bin/DevX.app" --args \
  --device=<id> --app=<identifier> --start-live
```

The window is rendered entirely in **Menlo** -- the face Terminal and Xcode are
read in, which ships with every macOS, so nothing is bundled and the licence
inventory stays empty. `--font=<family>` switches it (`--font=Monaco` for the
older Mac terminal face, or any coding font you have installed); a family that
is not installed is reported on stderr and the default is kept.

Write launch options as `--flag=value`. The space-separated form works too,
but AppKit pairs arguments differently than DevX does and can be left holding
a bare one, which it treats as a file to open -- see
[known limitations §5](docs/known-limitations.md).

Native sidebar over Devices · Apps · **Live** · Preflight · Record · Sessions ·
Issues · Detectors. The Live tab streams a capture alongside the running app --
frame and sample counts, memory sparklines per family, per-source status, and a
preliminary banner over all of it until the window closes. It renders the engine's output and nothing more: the provenance
banners, the separate detection and cause columns, the severity rationale and
the detector-execution table are all the model's own fields, so the UI cannot
present a softer story than the analysis.

On a host without SwiftUI, `devx-serve` serves the same views over loopback
HTTP (127.0.0.1 only, token required per start).

## Use

```bash
mpi devices                                    # discover Android + iOS devices
mpi apps --device <id> --running                # enumerate apps on one device
mpi preflight --device <id> --app <identifier>  # probe capabilities for a target
mpi record --device <id> --app <identifier>     # record a capture
mpi record --live --tick-ms 500                 # stream until Ctrl-C, one line per tick
mpi analyze <session|trace>                     # analyze and report
mpi compare <baseline.json> <candidate.json>    # compare two run sets
mpi rules                                       # describe every detector
mpi sdk-bridge                                  # accept SDK markers without recording
mpi export <session> --format json              # re-export a session
```

Add `--json` for machine-readable output, `--ci` to forbid prompting and
inferred targets. An unknown flag is a usage error rather than being ignored:
a flag that was silently dropped would change what got measured without saying
so.

`--live` keeps the window open until stopped, printing a line per tick with
its own cost. Frames and memory arrive on the tick; CPU arrives in background
windows because `simpleperf` costs about 5.6 s per record-and-symbolise cycle,
and the intervals between those windows are recorded as coverage gaps, not as
measured idle time. Without `--live`, `--duration-s` records a fixed window.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | ok |
| 2 | usage error |
| 3 | regression detected |
| 4 | inconclusive — including "every detector was skipped" |
| 5 | collection error |
| 6 | unsupported operation |
| 7 | ambiguous target |
| 8 | cancelled |
| 9 | not found |

`3` and `4` are deliberately distinct: a comparison that could not establish
anything is not a pass.

### Try it without a device

```bash
# A positive fixture: frame misses, a long JS task, a CPU hotspot, tooling cost
./build/bin/mpi analyze fixtures/traces/positive-frames-js-cpu.mpi.json

# A healthy capture: detectors RUN and find nothing (not the same as not running)
./build/bin/mpi analyze fixtures/traces/negative-healthy.mpi.json

# Incomplete evidence: proxy frames, unmapped clock, too few samples, drops
./build/bin/mpi analyze fixtures/traces/incomplete-evidence.mpi.json

# Real third-party formats
./build/bin/mpi analyze fixtures/traces/hermes-profile.json
./build/bin/mpi analyze fixtures/traces/chrome-trace-event.json
```

Every fixture is labelled `synthetic` and every report says so at the top.

---

## The in-app SDK

Screens, navigations, interactions and the build handshake come from the app
itself -- no device provider can see them, and a screen is never guessed from a
function name. `sdk/react-native/mpi-sdk.js` is dependency-free ES module
JavaScript with no build step; see
[its README](sdk/react-native/README.md).

```bash
mpi record --device <id> --app <identifier> --sdk --duration-s 20
# prints: SDK endpoint http://127.0.0.1:60773 token e5c7675b...
#         let the device reach it: adb reverse tcp:60773 tcp:60773
```

The endpoint binds 127.0.0.1 and requires that token on every request. A lost
batch becomes a recorded gap rather than silence, and a capture with no SDK
reports that it has no screen or interaction evidence instead of leaving the
fields quietly empty.

## Layout

```
apps/cli/            the `mpi` command-line interface
apps/devx-mac/       DevX.app -- native SwiftUI desktop app
sdk/react-native/    the in-app SDK: markers, build handshake, transport
apps/devx-serve/     the same views over loopback HTTP
core/capi/           C ABI the SwiftUI app is built on
core/
  model/             normalized trace, identity, capability, build, issue contracts
  discovery/         provider interface + reconciliation across adapters
  ingestion/         readers (native, Chrome trace-event, Hermes profile) + normalization
  symbols/           source maps, R8 mappings, path-traversal defence
  rules/             detector contract, registry, DET-01..05 and 07..12; DET-06 deferred
  report/            JSON and Markdown writers
  session/           session package on disk, collector contract, comparison
  util/              JSON, process execution, cancellation, time
adapters/android/    adb adapter, live capture collector, atrace/ftrace parser
adapters/ios/        devicectl / simctl / xctrace adapter
fixtures/
  traces/            labelled synthetic traces (positive, negative, incomplete, malformed)
  provider-output/    *.real.* = genuine tool output; *.synthetic.* = hand-written
  symbols/           source maps and R8 mappings, including hostile ones
tests/unit, tests/integration
docs/adr/            architecture decision records
docs/capabilities/   tested capability matrix, iOS toolchain probe record
sdk/, samples/       empty — M3
```

## Documentation

- [Known limitations](docs/known-limitations.md) — what is not true of this build
- [Tested capability matrix](docs/capabilities/tested-capability-matrix.md) — measured probe results
- [iOS toolchain probe record](docs/capabilities/ios-toolchain-probe.md) — the M0 schema validation
- [Requirement → test map](docs/requirement-test-map.md) — 109/198 checklist items, with reasons for the rest
- [Milestone report](docs/milestone-report.md) — M0/M1 in the format spec §21 asks for
- [Third-party licenses](THIRD-PARTY-LICENSES.md) — empty, deliberately
- ADRs: [core](docs/adr/0001-cplusplus-core-and-normalized-model.md) ·
  [no deps](docs/adr/0002-zero-third-party-dependencies.md) ·
  [no shell](docs/adr/0003-no-shell-argv-only-process-execution.md) ·
  [UI seam](docs/adr/0004-defer-the-desktop-ui-and-fix-the-seam.md) ·
  [honesty by types](docs/adr/0005-conservative-by-construction-model.md) ·
  [Android text sources](docs/adr/0006-android-text-sources-not-perfetto.md) ·
  [DevX in SwiftUI](docs/adr/0007-devx-is-a-native-swiftui-app.md)

## How to read a report

- `observed` means measured. `suspected` means the evidence is indirect or came
  from a proxy source. `inconclusive` means the detector could not decide.
- **Detection status and cause status are separate.** A measured symptom with
  `cause_status: unknown` is the normal, honest result.
- Severity orders impact. It is not confidence in a cause.
- A temporal correlation is a `candidate` cause, never a proven one.
- Missing data is a **gap**. It is never reported as zero.
- A `skipped` detector found nothing *because it did not run*. That is not the
  same statement as "no issue exists".
