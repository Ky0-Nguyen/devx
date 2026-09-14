# Mobile Performance Inspector (`mpi`)

A cross-platform mobile performance profiler with a C++20 analysis core.

Implements **M0 (both-platform feasibility)** and **M1 (C++ core and offline
analysis)** of `MOBILE_PROFILER_IMPLEMENTATION_SPEC_EN.md` v2.0.

> **Read this first.** Live on-device capture is **not implemented** in this
> milestone, and no physical device was reachable during development, so the
> live path is explicitly **unverified**. `mpi record` refuses to fabricate a
> capture. What works, what does not, and what was measured are in
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
| Live capture | ❌ M2 | ❌ M2 |
| Offline analysis, issues, evidence | ✅ | ✅ |
| JSON / Markdown reports | ✅ | ✅ |
| Benchmark comparison engine | ✅ | ✅ |

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

Run the tests (227 cases across 12 binaries):

```bash
cd build && ctest --output-on-failure
```

Build with ASan + UBSan — development only, never for benchmark capture
(spec §15):

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMPI_ENABLE_SANITIZERS=ON
cmake --build build-asan && (cd build-asan && ctest --output-on-failure)
```

The binary lands at `build/bin/mpi`. Warnings are errors by default
(`-DMPI_ENABLE_WARNINGS_AS_ERRORS=OFF` to relax).

### Platform prerequisites

Only needed for live discovery, not for offline analysis.

- **Android** — Android SDK Platform Tools on `PATH`; USB debugging enabled and
  authorized on the device.
- **iOS** — macOS with Xcode installed and selected (`xcode-select -p`). A
  physical device must be unlocked, trusted, and have Developer Mode enabled;
  Xcode must have prepared its developer disk image (the tool reports
  `ddiServicesAvailable: false` when it has not).

---

## Use

```bash
mpi devices                                    # discover Android + iOS devices
mpi apps --device <id> --running                # enumerate apps on one device
mpi preflight --device <id> --app <identifier>  # probe capabilities for a target
mpi analyze <session|trace>                     # analyze and report
mpi compare <baseline.json> <candidate.json>    # compare two run sets
mpi rules                                       # describe every detector
mpi export <session> --format json              # re-export a session
```

Add `--json` for machine-readable output, `--ci` to forbid prompting and
inferred targets.

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

## Layout

```
apps/cli/            the `mpi` command-line interface
core/
  model/             normalized trace, identity, capability, build, issue contracts
  discovery/         provider interface + reconciliation across adapters
  ingestion/         readers (native, Chrome trace-event, Hermes profile) + normalization
  symbols/           source maps, R8 mappings, path-traversal defence
  rules/             detector contract, registry, DET-01/02/04/12, deferred detectors
  report/            JSON and Markdown writers
  session/           session package on disk, comparison engine
  util/              JSON, process execution, cancellation, time
adapters/android/    adb adapter
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
  [UI deferral](docs/adr/0004-defer-the-desktop-ui-and-fix-the-seam.md) ·
  [honesty by types](docs/adr/0005-conservative-by-construction-model.md)

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
