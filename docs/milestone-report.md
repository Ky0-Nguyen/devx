# Milestone report -- M0 and M1

Reported in the format specification section 21 requires: what works with real
evidence, how it was tested on each platform, what is unsupported or
unverified, which checklist ids are done or deferred, and the next concrete
step.

Host: macOS 26.6.2, Apple M4 Pro, 48 GB RAM. Date: 2026-09-14.

> **This report is the record of the M0/M1 gate and is left as it stood.**
> Work has landed since, so several of its statements are no longer current.
> Superseded, with the section each one contradicts:
>
> - **Android live capture is implemented and verified** on an emulator, batch
>   and streaming, over the platform's text interfaces rather than Perfetto
>   (ADR-0006). Sections 2 and 3 below still list it as not implemented.
> - **Realtime streaming works**: `mpi record --live` and DevX's Live tab keep
>   the window open and show evidence as it arrives, with every pre-close
>   snapshot marked preliminary.
> - **The streaming ingest reader** named as step 1 of section 5 is done: the
>   same 1 GiB fixture takes 7.7 s and 5.15 GB peak, from 12.5 s and 10.2 GB.
>   Section 1's stress result is the before figure; `known-limitations.md`
>   section 3 has both.
> - **All twelve detectors are implemented**, not four. DET-03,
>   DET-05, DET-06, DET-07, DET-08, DET-09, DET-10 and DET-11 have landed
>   since, DET-06 over a real `am dumpheap` heap graph. Section 4 of
>   `known-limitations.md` is the current account.
>   On the healthy fixture the summary still reads `4 detector(s) ran, 0 found
>   something, 8 could not run`, but the eight are implemented detectors
>   skipping for want of evidence -- scheduling, SDK markers, a heap dump, a
>   launch, a run-set pair -- and each names the flag that would collect it,
>   not a delivery phase. Section 1's "eight unimplemented ones carrying its
>   phase and blocker" is the older state.
> - **The desktop UI exists.** DevX is a SwiftUI/AppKit app, so the "not
>   implemented (M2)" row in section 3 and ADR-0004's open framework question
>   are both closed.
> - **The app SDK exists for React Native** (`sdk/react-native`), with a
>   token-gated loopback transport and a build handshake, so section 6's
>   "`sdk/*` are empty directories" is false of all three: `sdk/ios` and
>   `sdk/android` each hold a README saying why there is no native SDK.
> - **DET-12 reports 23% on the positive fixture**, not the 16% section 1
>   quotes; the attribution it rests on -- 405 samples to
>   `react_native_dev_support` by a conclusive rule, the `metrocache` frame
>   left `name_match_only_not_attributed` -- is unchanged.
> - **Android app and process enumeration is verified**, against the same
>   emulator: 260 packages from `pm list packages -U`, 303 rows from `ps`,
>   `/proc/<pid>/stat` for process identity. Section 2's "absent-adb paths
>   only" and "never run against hardware", and section 3's "implemented, not
>   verified", predate it. Hardware, as distinct from an emulator, is still
>   unreached.
> - **Checklist coverage is 177 of 198**, not 109, with the 21 uncovered items
>   and their reasons in `docs/requirement-test-map.md`; that file supersedes
>   section 4's met and open lists, which are the M1-gate snapshot.
> - **Of section 4's open UI items, only J12 is still open.** A16, A23, A25
>   and I21 have tests (A16's keyboard-focus half is unverifiable here, since
>   the automation has no Accessibility permission), and I21 was measured
>   against the 1 GiB session. J14 is partially met, on an emulator; J15 is
>   open. J12 stays open because the build is ad-hoc signed and there is no
>   Developer ID in this environment.
> - **The M0 gate's second reason is gone; its first remains.** Collectors
>   exist on both platforms and have captured real telemetry from a real app
>   on an Android emulator and an iOS simulator. No physical device has been
>   reached on either, so the gate is still not met on hardware.
> - **The M2 gate is met on Android, against an emulator, and open on iOS**
>   (`known-limitations.md` section 1; ADR-0004). Section 4's "not met, and
>   not claimed" is the state at the M1 gate.
> - **iOS live capture exists for the simulator.** An app in a booted
>   simulator is an ordinary macOS process owned by the developer, so a host-
>   process collector streams its CPU time, utilisation, memory footprint and
>   per-thread times through `libproc`, with no root and no Instruments, and
>   `/usr/bin/sample` returns a symbolised call graph that becomes weighted
>   CPU samples behind an opt-in stack-profile checkbox in the Live tab.
>   `sample` reports an aggregate with no timestamps, so it says where and
>   never when, and nothing from it is placed on a timeline. There are no
>   frames: no command-line frame source exists for a simulator, and they are
>   reported as a missing provider, never as zero. Verified on the iPhone 17
>   Pro simulator only; the route does not exist for a device, where the app
>   is not a host process. An earlier version of the collector stated that
>   stack attribution was impossible, reasoning from `task_for_pid` being
>   refused; that was wrong, and `docs/ios-live-capture-findings.md` section 7
>   keeps the correction.
> - **A running app can be read without installing anything in it.** `mpi
>   inspect` and DevX's Inspect tab attach to the inspector a React Native
>   debug build already runs and report its network exchanges, console output
>   and Redux store -- the Reactotron questions, with nothing added to the
>   app. Request and response headers and bodies are kept only with
>   `--detail`, because an Authorization header carries a bearer token and the
>   report can be exported; a test asserts a token cannot reach a report
>   nobody asked to contain one. Values are verbatim, not redacted: a redacted
>   header is a claim about what was sent that is not true. Redux is two tiers
>   that claim differently. `--redux-watch` is read-only through
>   `store.subscribe`, which is how react-redux itself watches the store, and
>   names no action because Redux hands subscribers none. `--redux-actions`
>   also wraps `dispatch` in the running app, which modifies it, so it is opt-
>   in, stated in the report and put back afterwards; a thunk's injected
>   dispatch bypasses the wrapper and such a record says so. `--screenshot`
>   photographs the screen before and after; there is no physical-iOS route,
>   and it is reported unavailable rather than attempted. None of this is a
>   performance measurement -- attaching a debugger changes what the runtime
>   does, and the report says so. It is outside the M0/M1 scope this report
>   covers; `docs/inspect-without-installing.md` is the account.
> - **An empty iOS recording is diagnosed rather than reported as empty.**
>   Instruments samples through the unified log store, and a process without
>   Full Disk Access cannot open it; `xctrace` describes that as "the log
>   archive is corrupt or incomplete", which is not what is wrong. `mpi
>   preflight` now probes it as `ios.capture.log_store` -- one `log show
>   --last 1s`, reported as available or `permission_denied`, never unknown --
>   so the problem is found before a capture is attempted, and a capture that
>   hits it reports the permission and the fix instead of `limited`.
>   Separately, `time-profile` is not the only table that carries samples: a
>   recording on this host held 0 `time-profile` rows and 2 `time-sample`
>   rows, so an empty `time-profile` now triggers a row count over the tables
>   that plausibly carry samples and reports how many exist, in which schema,
>   that this build does not read it, and where the bundle is. Parsing `time-
>   sample` is not attempted, because the only specimen is two rows from a
>   recording that failed, and a parser that looks like support is worse than
>   none. Section 1's preflight paragraph predates the capability.
> - **A simulator or emulator can be started from the tool.** `mpi boot` and
>   the Devices tab keep "started" and "ready" apart and report the device id
>   observed afterwards rather than the AVD name. The wait loops' budget was a
>   lower bound on how long the wait could take, not an upper one: each poll
>   was handed its own fixed timeout after the budget was checked, so a 180 s
>   budget could run to 239 s against a wedged CoreSimulatorService, and a
>   poll that never answered was indistinguishable from "not booted yet", so a
>   broken tool was reported as a device that never came up. Each poll now
>   gets what is left of the budget, three unanswered polls stop the loop and
>   name the tooling failure and its fix, and the Android path refuses to
>   start when it cannot take a baseline `adb devices` snapshot, since an
>   empty baseline would report someone else's emulator as the one it started.
>   Pinned by a stub that wedges. `known-limitations.md` section 11 has what
>   was measured.
> - **A v0.1.0 build has been cut** (`docs/RELEASE-v0.1.0.md`): a `DevX.dmg`
>   produced by the `devx_dmg` target, Apple Silicon only, macOS 14 or newer.
>   It is ad-hoc signed and not notarized, so Gatekeeper refuses it on first
>   launch with a message that sounds like a damaged file; the notes lead with
>   the right-click-Open step because that refusal is the most common way a
>   tool like this gets written off as broken. J12 is unchanged by the
>   release: there is no Developer ID in this environment.
> - **The test count is 543 cases in 23 C++ binaries, plus 502 Swift checks and
>   78 smoke checks**, not 228 in 12 -- read from the binaries on 2026-09-16,
>   not remembered. Section 1's 24 comparison-engine cases are 40 now.
>
> **Still open**, unchanged from this report: no physical device has been
> reached on either platform, and no iOS recording has ever completed. An
> `xctrace` collector now exists and its export path is tested against real
> Instruments output, but `xctrace record` does not terminate against the
> simulator on this host. iOS live capture exists by a different route: a
> host-process collector streams CPU time, utilisation, memory footprint and
> per-thread times from an app in a booted simulator, and `/usr/bin/sample`
> gives it stack attribution -- verified on the simulator, and not possible on
> a device, where the app is not a host process. So of the two reasons the
> gate assessment gives, "no collector exists" no longer holds on either
> platform and "no physical device was reachable" still does.
>
> `docs/known-limitations.md`, `docs/capabilities/tested-capability-matrix.md`
> and, for everything iOS, `docs/ios-live-capture-findings.md` are the current
> authorities. The last was rewritten on 2026-09-15 to state the current
> state first and keep its corrections as history.

---

## 1. What works, with real evidence

### Cross-platform device discovery -- works, verified against real tooling

`mpi devices` on this host returns, in one list:

```
PLATFORM  FORM       STATE        DEVICE ID                              OS             NAME
ios       physical   offline      3FF46431-775C-59BB-AD26-D316DFAFA5A6   26.0 (23A340)  QuocBao's iPhone
ios       physical   offline      6769FAD1-FC2F-5EDD-878B-AEC7BB056048   18.5 (22F76)   iPad Air (Pizza Hut)
ios       simulator  authorized   456FA0D8-48C1-4BEC-B087-50E8A046EA5D   26.5           iPhone 17 Pro
... 24 further simulators ...
```

The evidence that this is real and not cosmetic:

- The two physical devices are reported **`offline`, not absent and not
  usable**. Both had `connectionProperties.tunnelState: "unavailable"`. A
  naive read of `pairingState: "paired"` alone would have offered them for
  capture.
- Exactly one simulator is `authorized`, matching the one booted simulator.
- Android: `adb devices -l` returned only its header, and the parser yields
  **zero** devices rather than a phantom entry.
- Simulators are listed but never merged with physical devices; `DeviceForm`
  keeps them separate everywhere downstream.

### App enumeration and selection by identifier -- verified on iOS simulator

`mpi apps --device 456FA0D8-... --running`:

```
STATE      PROFILING            SCOPE                  PROCS  IDENTIFIER
running    unknown              partial                1      com.apple.Spotlight
running    unknown              partial                1      com.apple.chrono.WidgetRenderer-Default
running    unavailable          complete_for_provider  1      com.apple.mobilecal   (Calendar)
3 of 29 app(s) shown.
```

This reconciles **two independent providers**, as spec section 3.3 requires:
`simctl listapps` for the installed inventory and `launchctl list` for live
processes. The first two rows are `partial` scope precisely because launchd
names a bundle id that the installed listing does not contain — so the tool
reports them and says its listing is incomplete, rather than hiding them or
claiming completeness.

Ownership for the running entries is `provider_attributed`: the launchd job
label *is* `UIKitApplication:<bundle-id>[...]`, which is the OS attributing the
process to the app. That is the strongest evidence tier, and it is not a name
match.

### Offline analysis with evidence-backed issues -- works

`mpi analyze fixtures/traces/positive-frames-js-cpu.mpi.json` produces:

- **DET-01**: "12 of 90 frames missed their deadline on surface 'MainActivity'",
  `detection_status: observed`, `cause_status: unknown`, with the interval
  narrowed to the worst 122 ms consecutive run and ten resolvable frame
  references. The deadline came from the observed 120 Hz refresh interval
  (8.33 ms), not from a constant.
- **DET-02**: "Long JS execution: 'recomputeCartTotals' ran 180 ms",
  `observed` / **`candidate`** — candidate and not supported, because the JS and
  UI clocks have a *measured* mapping and frames missed inside the interval,
  which is a correlation. The issue lists "a third factor could have caused
  both" among its alternatives and asks for a controlled re-measurement.
- **DET-04**: "one unresolved frame holds 76% of samples on thread 'main'" —
  it refuses to print a function name because no symbols were supplied, and
  says so in `missing_evidence`.
- **DET-12**: attributes 16% of main-thread samples to
  `react_native_dev_support` via a conclusive ownership rule, while a
  `com.example.perf.metrocache.Loader.load` frame that merely *contains*
  "metro" is reported as `name_match_only_not_attributed` under
  `shared_or_unknown`.

### The three honesty behaviours that matter most -- all verified end to end

**"Found nothing" is distinguishable from "did not run".** On the healthy
fixture: `4 detector(s) ran, 0 found something, 8 could not run`, with all four
implemented detectors recorded `ran_found_nothing` and each of the eight
unimplemented ones carrying its phase and blocker.

**Degraded evidence degrades the conclusion.** On the incomplete fixture,
DET-01 drops from `observed` to `suspected` because the frame source is a
display-callback proxy; it excludes the 15 frames with no presentation
timestamp from its denominator rather than counting them as on-time; DET-02
holds `cause_status: unknown` because the clock mapping is present but
`measured: false`; and DET-04 skips entirely with "only 12 sample(s) ... a
share computed from this few samples would be noise".

**An unknown is never a pass.** `mpi preflight` on this host reports benchmark
eligibility `insufficient_evidence` with all seven unknown dimensions
enumerated, not the first one.

### Comparison engine -- works

24 test cases covering: both-threshold gating, too-few-runs, high-variance,
zero-baseline division, and every condition mismatch (device, form, OS,
refresh, collector preset, sample rate, launch class, thermal, power, cache,
network, input data). A cross-platform pair is refused as a regression gate
outright, and a crashed run is excluded rather than counted as an impossibly
fast success.

### Measured stress result

1.0 GiB normalized trace, 2,920,000 events: ingested completely in **12.5 s**
at **10.2 GB peak RSS**. The time is fine; the 10× memory blowup is a real
defect, recorded with its cause and remediation in
`docs/known-limitations.md` §3. It also exposed that the default input ceiling
(512 MiB) refused the specification's own 1 GiB fixture — now 2 GiB and
overridable.

---

## 2. How it was tested, per platform

**228 test cases in 12 binaries. 12/12 green under both RelWithDebInfo and
Debug+ASan+UBSan.** ASan caught two real defects during development (a
use-after-free in a test, and a heap issue), and `-Werror` with
`-Wconversion -Wsign-conversion -Wold-style-cast -Wshadow` caught a malformed
boolean condition in the Chrome trace reader.

| Layer | Android | iOS |
|---|---|---|
| Host-tool output parsing | real `adb --version`, real empty `adb devices -l` | **real** `devicectl list devices` JSON, **real** `simctl list devices` JSON, **real** `simctl listapps` (plist→JSON), **real** `launchctl list` |
| Device-side output parsing | hand-written `.synthetic.` fixtures — `ps`, `pm list packages -U`, `/proc/pid/stat`, `dumpsys` | n/a |
| Adapter against live toolchain | absent-adb paths only | `probe()` and `list_devices()` run against the real installed Xcode toolchain in CI-style tests |
| Live app enumeration | ❌ never run against hardware | ✅ against the booted simulator |
| Live capture | ❌ not implemented | ❌ not implemented |

Three defects found by tests and fixed during this milestone:

1. **DET-12 dropped unclassified-only buckets.** A thread whose only
   tooling-shaped frames were name-only matches produced no attribution at all,
   silently losing the "saw something, could not attribute it" fact that spec
   §9 rule 5 wants visible.
2. **R8 deobfuscation checked the wrong thing first.** It split the last
   segment off before checking whether the whole string was itself a class
   name, so `a.b.c` → looked up non-existent class `a.b`.
3. **The negative fixture was borderline, not negative.** Seven uniform CPU
   leaves gave ~14.3% shares against a 15% threshold; three crossed it. The
   detector was right and the fixture was wrong — regenerated with 14 leaves.

---

## 3. Unsupported and unverified, with prerequisites

| Capability | State | Prerequisite to change it |
|---|---|---|
| Live capture, both platforms | **not implemented** (M2) | a Perfetto collector and an `xctrace` collector wired to a session controller |
| Android app/process enumeration | implemented, **not verified** | any authorized Android device |
| iOS physical app/process enumeration | implemented, **not verified** | a reachable device with `ddiServicesAvailable: true` |
| Deep attach permission | `unknown`, per-app | a development-signed build; never bypassed |
| Desktop UI | not implemented (M2) | a framework decision (ADR-0004 keeps it open) |
| App SDK, markers, build handshake | not implemented (M3) | `sdk/*` are empty directories |
| Memory / IO / network / lock / React detectors | registered, always skipped | the corresponding collectors |
| 8 of 12 detectors | registered, always skipped, each with its stated phase | see `mpi rules` |

Nothing in this table is reported as working. Every row corresponds to a
`not_tested` or `unknown` entry in the capability matrix.

---

## 4. Checklist status

**109 of 198** section-18 items have automated coverage; the other 89 are
listed with a stated reason in `docs/requirement-test-map.md`.

Notable items **met**: A01–A05, A08, A11–A13, A15, A18, A20, A24 (discovery
semantics) · B02–B04, B06–B10, B12–B13 (identity) · C01, C03–C04, C06–C07,
C09–C11, C13–C15, C17–C19 (build/eligibility) · D04, D08–D12, D16, D18–D19
(trace integrity) · E01–E02, E12–E15, E20–E22 (frames/CPU semantics) ·
F11, F13–F14, F17 (attribution) · G01, G11, G14, G16–G19 (symbols) ·
H01–H07, H09–H12, H15–H17 (rules/reports) · I01–I05, I07–I17, I19 (comparison) ·
J01–J05, J09–J11 (security).

Notable items **open**: **J14** (real Android discovery + live capture) and
**J15** (real iOS live capture) — the two that gate the cross-platform
Definition of Done. Also every A/B/C item needing hardware, every D item
needing a collector, all F memory items, and the UI items A16/A23/A25/I21/J12.

### Gate assessment, stated plainly

- **M0 gate** — partially met. Real device discovery works on both platforms
  and installed/running enumeration scope was validated independently. Not met:
  capturing permitted real telemetry for one owned app on each platform, because
  no physical device was reachable and no collector exists.
- **M1 gate** — met. Both fixture families produce truthful evidence, and
  missing data never becomes a pass. This is the milestone actually delivered.
- **M2 gate** — not met, and not claimed.

---

## 5. The next concrete implementation step

**Wire one real collector on one platform, end to end, and keep the ingest
streaming.**

Specifically, in this order:

1. **Replace the DOM parse on the ingest path with a streaming reader.** The
   10× memory blowup (§3 of known-limitations) will otherwise be inherited by
   every real capture, which will be far larger than a 1 GiB fixture. The
   `Reader` interface already hides the strategy, so this touches only
   `core/ingestion`.
2. **Implement `AndroidCollector` over `adb shell perfetto`** with a real
   trace config, feeding `SessionController` through the state machine that
   `SessionState` already models. Android first only because `adb`'s
   authorization model is the easier of the two to get a device into — iOS
   follows immediately, and the cross-platform requirement is not deferred.
3. **Produce a measured clock mapping** between the collector's clock and the
   JS clock. This is what lets DET-02 reach `candidate` on real data instead of
   always declining.
4. **Then run the M2 gate on one physical device per platform** and convert the
   `not_tested` rows in the capability matrix into measured results.

The analysis engine, the issue contract, the reports and the comparison engine
do not need to change for any of this — which was the point of building them
first.
