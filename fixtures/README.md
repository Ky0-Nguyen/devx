# Fixtures

## Naming discipline

The suffix is the provenance claim, and it is load-bearing:

- **`*.real.*`** -- genuine output captured from this build host's tooling on
  2026-09-14. Stored verbatim, except that the measured app's identifiers
  were renamed afterwards: its package and bundle id read
  `com.acme.shopper.debug`, its name `Shopper`, its backend hosts and the
  signed-in username are placeholders. Every number is as recorded.
- **`*.synthetic.*`** -- hand-written. No hardware produced it.
- Everything in `traces/` is a synthetic trace whose JSON carries
  `"synthetic": true` and a `synthetic_note`. The analysis engine propagates
  that flag to every derived event, metric and evidence reference, and both
  report formats announce it at the top.

Specification section 0.6 requires fixtures and illustrative values to be
labelled, and section 0.5 forbids presenting a dashboard over synthetic data as
a working profiler. The naming is how that is enforced at a glance.

## `traces/`

| File | Purpose |
|---|---|
| `positive-frames-js-cpu.mpi.json` | Positive: exercises DET-01, DET-02, DET-04, DET-12 together. 120 Hz presentation timestamps with a 12-frame miss run, a 180 ms JS task on a *measured* clock mapping, a dominant CPU leaf, and both conclusive and name-only tooling matches. |
| `negative-healthy.mpi.json` | Negative: 60 Hz with no misses, short JS tasks, and CPU spread across 14 leaves so no share approaches the hotspot threshold. All four implemented detectors must *run* and find nothing. |
| `incomplete-evidence.mpi.json` | Incomplete: a display-callback proxy frame source, 15 frames with no presentation timestamp, a variable refresh rate, an *unmeasured* clock mapping, an unterminated JS span, 12 samples (below threshold), 5120 provider drops, and a partial capture. |
| `obfuscated-android.mpi.json` | Stack frames are R8-obfuscated (`a.b.c.d`), as a real release-built Android app produces, and the trace declares `native.build_id`. Exercises deobfuscation through the analysis engine: without a mapping the raw string must not be presented as a function name, and with a bound mapping the location still stays `partial` because an R8 map yields no source file. |
| `ambiguous-ownership.mpi.json` | Process ownership is `ambiguous`, so its events must be excluded from app-scoped totals and the exclusion reported. |
| `empty-capture.mpi.json` | Structurally valid, zero events. Must not crash and must not read as "nothing happened". |
| `malformed-truncated.json` | Truncated mid-object. |
| `malformed-not-json.bin` | 256 arbitrary bytes. |
| `malformed-empty.json` | Zero bytes. |
| `chrome-trace-event.json` | Real Chrome/Perfetto trace-event format, including a closing B/E pair, an unclosed B, an orphan E, an X with no `dur`, an out-of-order timestamp, an instant event and a counter. |
| `android-cold-launch.real.mpi.json` | **Real capture**, not synthetic: a cold launch of the Shopper debug build on a booted emulator, recorded with `mpi record --launch`. Carries the two startup markers (`app_launch` from `am start -W` TotalTime, `startup_displayed` from the platform's `Displayed` log) both reporting 5239 ms, plus five memory families. Exercises DET-07, including the case where two endpoints agree and must group into one finding. |
| `android-screen-cycles.real.mpi.json` | **Real capture**: five mount/unmount cycles of one screen, driven through the app SDK during a live Android capture, with 53 memory readings and a *measured* clock mapping from the app's `performance.now()` to the device's boot clock (±11 ms). Its memory is flat, so it is DET-05's negative case: the rule must run and find nothing. |
| `android-react-and-network.real.mpi.json` | **Real capture**: an interaction that ran for 637 ms while one component committed 30 times and a request covered 99% of the interaction's span, all reported by the app SDK during a live Android capture. Exercises DET-10 and DET-11 on genuine data, including the mapped-interval path (the markers are on the app's clock, the capture is on the device's). |
| `positive-memory-growth.mpi.json` | **Labelled synthetic**, derived from the capture above. The memory series are rewritten so the families disagree: rss/pss/native_heap accumulate every visit, `dalvik_heap` stays flat, and `private_dirty` rises over the first two visits then settles. Exercises DET-05's positive path, its per-family separation (F05, F08) and its warm-up refusal (F02). Markers, clock mapping and sampling cadence are the real ones; the memory values are not. |
| `hermes-profile.json` | Real Hermes sampling-profiler shape (`stackFrames` + `samples`), including a self-referential parent chain that must not hang the unwinder. |

The 1 GiB stress fixture required by section 15 is **not committed**. Generate
it with `python3 tools/gen-stress-fixture.py <path>`.

## `provider-output/`

| File | Provenance |
|---|---|
| `devicectl-list-devices.real.json` | real `xcrun devicectl list devices --json-output` |
| `simctl-list-devices.real.json` | real `xcrun simctl list devices --json` |
| `simctl-listapps-booted.real.json` | real `xcrun simctl listapps`, converted from its NeXTSTEP plist with `plutil` |
| `simctl-launchctl-list.real.txt` | real `xcrun simctl spawn <udid> launchctl list` |
| `adb-devices-l.real.txt` | real `adb devices -l` with **no device connected** -- the header-only case |
| `adb-version.real.txt`, `xctrace-version.real.txt`, `devicectl-version.real.txt` | real version output |
| `android-gfxinfo-framestats.real.txt` | real `dumpsys gfxinfo PKG framestats` from a booted Android emulator, 43 frame rows at a platform-reported 60 Hz |
| `android-simpleperf-report-sample.real.txt` | real `simpleperf report-sample --show-callchain` from the Shopper debug build, including the `meta_info` block that carries `app_type: debuggable` |
| `android-simpleperf-not-debuggable.real.txt` | real simpleperf refusing a non-debuggable package -- the case the capability contract exists to report honestly |
| `android-meminfo.real.txt` | real `dumpsys meminfo`, including the swap situation that makes PSS exceed RSS |
| `android-atrace-cold-start.real.txt` | real `atrace -t 6 -b 16384 sched disk am view` across a cold start of the Shopper debug build, thinned to a 0.24 s window around the app's first uninterruptible block. Carries the evidence the scheduling detectors need and nothing synthetic: the app's **main thread in `D` state**, five of its threads blocked with `iowait=1` and a kernel `caller=` (`folio_wait_bit_common`, a page-cache read), 164 `sched_waking` lines identifying wakers, userspace slices including one with **no name at all**, the ftrace buffer accounting, and the kernel's own clock-sync pairing. 27 lines from other processes are kept so attribution has something to exclude. |
| `android-am-start-w-cold.real.txt` | real `am start -W` for a genuine cold launch: `LaunchState: COLD`, TotalTime 4889 |
| `android-am-start-w-already-running.real.txt` | real `am start -W` re-launching an app that is already foreground. Prints `TotalTime: 0` with a warning that no activity was started -- the zero that must never become a zero-millisecond startup |
| `android-displayed-logcat.real.txt` | real `ActivityTaskManager: Displayed ... +4s889ms`, the platform's own first-frame figure |
| `android-resolve-activity.real.txt` | real `cmd package resolve-activity --brief`, whose component sits on the last line under the resolution details |
| `xctrace-toc.real.xml` | real `xcrun xctrace export --toc`. Recorded against the **host Mac**, on purpose: it is the case where the tool must refuse to promote a macOS recording to iOS evidence. Also carries 23 exportable tables and two processes besides the target, so "unread" stays distinguishable from "empty" |
| `xctrace-time-profile.real.xml` | real `xcrun xctrace export --xpath '.../table[@schema="time-profile"]'`, trimmed to the first 12 of 954 rows. Exercises the format's reference table: 117 `<frame ref>`, 67 `<binary ref>` and every column kind appearing once by id and then by reference. Every reference resolves inside the fixture, so an unresolved one in a test means the reader lost it |
| `android-ps-A.synthetic.txt` | **hand-written.** No Android device was available. Covers a main process, two sub-processes, an isolated process, a shared-uid sibling, a same-name different-uid process, a work-profile instance and an unlisted process. |
| `android-pm-list-packages-U.synthetic.txt` | **hand-written.** Includes two packages sharing uid 10234. |
| `android-adb-devices-l.synthetic.txt` | **hand-written.** Authorized, emulator, wireless, unauthorized and offline devices. |
| `android-proc-stat.synthetic.txt` | **hand-written** `/proc/<pid>/stat`. |

## `symbols/`

| File | Purpose |
|---|---|
| `matching.map.json` | v3 source map whose bundle id matches the expected build. Mappings are genuine VLQ. |
| `mismatched.map.json` | Same map, different bundle id -- must be reported `mismatch` and never navigated. |
| `traversal.map.json` | Names `../../../../../../etc/passwd` as a source -- must be rejected. |
| `malformed.map.json` | Corrupt VLQ -- must be refused, not approximated. |
| `wrong-version.map.json` | `version: 2` -- unsupported. |
| `r8-mapping.txt` | Real ProGuard/R8 format with a `pg_map_id` header. |
| `r8-mapping-other-build.txt` | Same classes, different build id -- `mismatch`. |
| `checkout/` | A minimal local source tree so an exact match can resolve to a file that actually exists. |

## `runsets/`

Benchmark comparison inputs for `mpi compare`: an Android baseline, a clear
regression, a no-change candidate, and an iOS candidate used to prove a
cross-platform pair cannot drive a regression gate.

## `layout/`

| File | Purpose |
|---|---|
| `ios-probe-expo-go-home.real.json` | **Real**: the layout probe's answer from Expo Go on an iPhone 17 simulator (iOS 26.0), relaunched with `mpi layout --relaunch` on 2026-10-03. Three windows, 87 views, the SwiftUI home screen at the top of its navigation stack, and the keyboard's text-effects window that must not count as a screen. |
| `android-dumpsys-activity-top-settings.real.txt` | **Real**: `adb shell dumpsys activity top` with Settings in the foreground on a Pixel 6a emulator (API 33), 2026-10-03. Holds the launcher's activity too, so selecting by package is exercised. |
