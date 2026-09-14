# Known limitations

This is the register the specification asks for. It is written to be read by
someone deciding whether to trust a result, so it states what is **not** true
of this build as plainly as what is.

Nothing here is a surprise discovered at the end. Each entry has a phase and,
where one exists, a concrete remediation.

---

## 1. iOS live capture is not implemented; Android is

**Android works, batch and live.** `mpi record`, `mpi record --live` and
DevX's Live tab all capture for real, using the platform's text interfaces
(ADR-0006): `dumpsys gfxinfo framestats` for frames with a platform-supplied
deadline, `simpleperf` for symbolised stacks, `dumpsys meminfo` for memory.
Verified against a booted emulator (API 37) on the superapp HutBot debug
build, and live against `com.android.settings` for the frame path.

**iOS records, and on this host it does not finish.** There is an `xctrace`
collector wired to the session controller now, and an importer
(`ios.xctrace.export`) for what Instruments exports. The recording step was
exercised against the booted simulator with a real app
(`io.pizzahut.hutbot.debug`): `xctrace record` attaches -- it prints
`Attaching to: ... Time limit: 3.0 s` -- and then runs indefinitely, ignoring
its own `--time-limit` even with `--no-prompt`. The collector bounds the
recording itself and reports that as a **provider failure**, explicitly not as
a capture that found nothing, and writes no session.

So the honest state is: discovery, app enumeration, process identity,
preflight, the recording command and the export-and-read path are all
implemented for iOS; a **completed** iOS recording has never been produced in
this environment, on a simulator or a device. The successful branch of
`XctraceCollector::capture` is therefore code that has not run end to end, and
`ios.capture.live_recording` reports `tested: not_tested` unless discovery
established the device form.

What *is* verified is the ingestion: the importer is tested against real
`xctrace export` output for both shapes (`--toc` and a `time-profile` table),
including the reference table that makes frames and binaries repeat, and the
case where the export records platform `macOS` and must not be promoted to
iOS evidence.

**Why it is not disguised.** Spec section 0.16 forbids substituting
import-only support for the required live workflow, and section 0.5 forbids
presenting a dashboard over synthetic data as a working profiler. A session
package with no collector output would be a capture-shaped file containing
nothing measured.

**Consequence.** The M2 gate is **met on Android** (against an emulator, not a
physical device -- see section 2) and **open on iOS**. Checklist item J15 is
open; J14 is partially met.

**Phase.** M2 for the iOS collector.

### What the Android collector does not collect

No scheduling states, no I/O, no network. DET-03, DET-05, DET-09 and DET-11
therefore stay registered and skipped rather than being approximated from what
is available. Getting scheduling data means taking on Perfetto's protobuf;
ADR-0006 says when that trade becomes the right one.

A live capture does produce a memory *series* -- one reading per tick per
family -- but each reading is still an instant, so nothing is observed between
ticks. The per-source status says so on every capture, and the two limitations
below say what else the series cannot promise.

### What a live capture can and cannot claim

**Memory points are placed by the host, not the device.** `dumpsys meminfo`
reports no timestamp. The device's boot time is read once at capture start
(`/proc/uptime`) and paired with the host's steady clock; every later point is
that base plus host elapsed time, recorded as a clock mapping whose method and
10 ms resolution travel with the trace. A point's position on the timeline is
therefore accurate to about 10 ms plus drift, not exact. If the boot-time read
fails, the memory samples are dropped and the source says why -- they are never
stamped with a neighbouring event's time, and never with zero. An earlier
version did exactly that, and the opening samples of every capture landed
before the window began.

**CPU lags the tick.** `simpleperf` costs about 5.6 s per record-and-symbolise
cycle, so it runs on its own thread in windows while frames and memory stream
at the tick. CPU numbers arrive in batches, and the intervals between windows
are coverage gaps -- not measured idle time. A 25 s live capture typically
shows CPU coverage around 60%.

**Frames can be lost between ticks.** `framestats` is a ring buffer of about
120 frames. A read that comes back full with nothing seen before means frames
rendered and were never collected; that stretch is recorded as a coverage gap
and a shorter `--tick-ms` is the fix.

**An empty frame buffer is not a claim of zero jank.** `framestats` returning
no rows cannot distinguish an app that drew nothing from a source that returned
nothing, so the window is marked uncovered either way and the source status
carries which. Verified: profiling an app parked on a static screen reported 0
frames while the platform's own counter also said 0.

**Preliminary means preliminary.** Every snapshot taken before the window
closes is labelled: a detector that has found nothing may still fire, and a
finding may change as more evidence arrives. Nothing in a live view is a final
result.

---

## 2. No *physical* device was reachable; Android is verified on an emulator

**Android: emulator, not hardware.** Everything is verified against a booted
Android emulator -- discovery, app enumeration, process identity, and live
capture. An emulator is not a phone: its GPU is emulated, its scheduler is the
host's, and `perf` hardware counters are unavailable (which is why the
collector uses the `cpu-clock` software event). The capability matrix therefore
records these as `verified_on_simulator_or_emulator`, never as
`verified_on_physical_device`.

That distinction was itself a bug at one point: the probe hardcoded
`verified_on_physical_device` whenever it succeeded, so an emulator-only run
claimed hardware verification. Spec J18 and C20 forbid exactly that, and
`test_android_parsers` now asserts it cannot recur.

**iOS.** Two iPhones/iPads are paired with this host, and both reported
`connectionProperties.tunnelState: "unavailable"` throughout. The tool
correctly reports them as `offline` rather than absent or usable. Physical-device
app and process enumeration were therefore never exercised; both devices also
reported `ddiServicesAvailable: false`, which is itself the gate for those
operations.

**What WAS verified on real tooling.** Device discovery on both platforms,
and full app enumeration on a booted iOS simulator (iPhone 17 Pro, iOS 26.5)
including the `simctl listapps` plist conversion and `launchctl list` bundle-id
attribution. See `docs/capabilities/tested-capability-matrix.md` for the
per-capability breakdown.

**Simulator results are never treated as device results.** `DeviceForm` keeps
them separate, `evaluate_eligibility` marks a simulator run benchmark-ineligible,
and `compare` refuses a simulator-vs-physical pair outright.

**Remediation.** Connect a device and run `mpi preflight --device <id> --app
<identifier>`. Every `not_tested` row that hardware can answer will become a
measured result.

---

## 3. The 1 GiB stress fixture costs 5 GB of memory (down from 10 GB)

**Measured on this host** (macOS 26.6.2, Apple M4 Pro, 48 GB RAM), with a
1.0 GiB normalized trace of 2,920,000 events:

| | before streaming | after |
|---|---|---|
| wall time, ingest + normalize + analyze + export | 12.5 s | **7.7 s** |
| peak resident set size | 10.2 GB | **5.15 GB** |

`json::StreamParser` walks the document instead of materialising it, and the
reader converts one array element at a time so each element's DOM dies before
the next is read. Unrecognised members are skipped without being built at all.

**What remains.** About 1 GB is the file buffer, and the rest is the retained
`model::Event` vector: `sizeof(Event)` is 416 bytes, so 2.92M of them is
1.2 GB before their heap strings, plus allocator retention. Going lower means
either not retaining every event or reading the file in chunks, both of which
are larger changes than the streaming parse was.

**A measurement that disproved a guess.** I assumed `std::vector` growth
dominated the remainder and added an exact-reserve counting pre-pass. Measured:
27% slower for 4% less memory. Reverted.

**Also.** The default input ceiling was 512 MiB, which *refused* the
specification's own 1 GiB fixture. Now 2 GiB, overridable with
`mpi analyze --max-input-mib`.

**Related checklist.** I21 asks about UI responsiveness under the stress
fixture. DevX now exists, but it has not been driven against a 1 GiB session;
the ingest cost above is what is measured.

---

## 4. Five of the twelve catalog detectors are registered but not implemented

Implemented: **DET-01** (frame deadlines), **DET-02** (long JS), **DET-04**
(sampled CPU hotspot), **DET-05** (memory growth across screen cycles),
**DET-07** (startup budget), **DET-08** (regression), **DET-12** (tooling
attribution).

Registered and always skipped, each with its prerequisites, its phase and the
conclusion it will be allowed to reach: DET-03 (sync main-thread I/O), DET-06
(retention), DET-09 (lock contention), DET-10 (React renders), DET-11 (network
delay).

**Why register them at all.** Spec H05 requires an unsupported rule to be
*reported as skipped* and H11 requires "no findings" to be distinguishable from
"no analysis". A detector the engine has never heard of can satisfy neither.
Every report therefore lists all twelve with an explicit outcome.

**DET-05 will not say "leak".** The strongest conclusion the spec allows it is
*suspected retention*, and the rule is built around that: rising memory across
screen cycles is equally consistent with a cache filling up, garbage that has
not been collected, an allocator holding freed pages, or a screen legitimately
keeping more state. All four travel with every finding, severity is capped at
medium so an inferred finding cannot outrank a measured one, and the missing
evidence names what would settle it (reference paths, which is DET-06).

It also refuses two comparisons. It never sums memory families -- each is
judged separately and a finding names which one grew (section 8) -- and it
will not compare the app's marker clock against a device measurement without
a **measured** mapping between them, because an assumed offset would make a
correlation look real. And it distinguishes warm-up from accumulation: a
series that rises over the first visits and then settles is reported as
warm-up settling, not as suspected retention.

**DET-10 reports a pattern, not a defect.** The spec's allowed conclusion is
"pattern, not automatically defect", so severity is capped at low, the finding
says in its own words that a defect is not established, and the metric says a
count is not a cost. It also refuses the obvious substitute: CPU samples that
land inside React measure where time went, not how many times a component
committed, and the skip names that rather than making do.

**DET-11 blames nobody.** A request duration covers DNS, the TLS handshake, a
cold radio, retries, proxies, time queued behind other requests in the app,
and the app's own delay reading the response -- as well as the server. All of
them are listed as missing evidence on every finding, the cause status stays
`candidate` because a temporal overlap is not a demonstrated dependency, and
the severity rationale says explicitly that it is not a claim about the
network or the server.

**DET-07 will not invent a budget.** A startup budget is a product decision and
no platform publishes one, so the rule skips until `DET-07.budget_ms` is
configured -- and says that the *budget* is missing, not the data. It also
refuses to average its two endpoints: `am start -W`'s TotalTime ends when the
activity reported being drawn, the platform's `Displayed` line marks the first
frame, and neither is the moment the app became usable. Each endpoint is named
in the finding; endpoints that report the same figure group into one finding
with two witnesses rather than two apparent problems.

**DET-08 runs over a pair, not a capture.** Its evidence is a baseline and a
candidate run set, so it skips in a single-session analysis saying exactly
that. It does not decide significance -- the comparison engine does -- and it
refuses cross-platform pairs and incomparable conditions outright instead of
reporting a qualified finding.

---

## 5. DevX exists; some UI behaviours are still unbuilt

`DevX.app` is a native SwiftUI application over the C ABI (ADR-0007), covering
Devices, Apps, Live, Preflight, Record, Sessions, Issues and Detectors. The
Live tab streams a capture alongside the running app: counters, memory
sparklines, per-source status, and the preliminary banner over everything
until the window closes.

Still open, and all inherently UI behaviours:

- **A16** selection and keyboard focus preserved across a list refresh. The
  selected *device* is preserved (and never silently switched); focus is not
  managed.
- **A23** favourites and recents.
- **A25** responsiveness with a very large app list. The emulator's 266 apps
  render fine; nothing larger has been tried.
- **I21** responsiveness under the 1 GiB stress fixture.
- **J12** signing and packaging for distribution. The bundle is ad-hoc signed
  for local use only.
- No timeline view. Spec section 13 lists one; issues currently carry their
  interval numerically rather than on a rendered track.
- No compare view. `mpi compare` is CLI-only.
- The Live tab has no frame-timeline track either; it shows counts, source
  status and memory series.
- Every label in the window is Menlo (or `--font=<family>`), except the window
  title in the title bar, which AppKit draws in the system face.

**Launching with arguments.** AppKit reads the process argument vector itself
and treats anything it cannot pair with a `-flag` as a document to open. DevX
has no document scene, so such a launch made SwiftUI skip creating the window
altogether -- a live process with a healthy run loop, zero windows, and
`onAppear` never firing. `DevX --start-live --live-seconds 30` triggered it,
because AppKit reads `--live-seconds` as the value of `--start-live` and is
left holding `30`. DevX now hands its options over in the environment and
re-executes itself with a clean vector; `--flag=value` is accepted too and is
the safer form to script.

---

## 6. The app SDK exists; React render and network data depend on the app calling it

`sdk/react-native/mpi-sdk.js` is the in-app SDK: plain ES module JavaScript,
no dependencies, no build step. It sends the markers spec section 11 names --
screen mount/unmount, navigation begin/end/**cancel**, interactions, async
spans -- plus a build/runtime handshake carrying the build configuration, the
JS engine and version, the bundle id, the OTA update id and `__DEV__`.

The endpoint (`mpi record --sdk`, or `mpi sdk-bridge` on its own) binds
127.0.0.1 and requires a token on every request. There is no wireless path.

**Verified end to end**: the real JavaScript client drives the real C++ ingest
over real HTTP in `sdk/react-native/test/e2e.mjs` (33 checks, run by the smoke
test when node is present), and a real emulator capture carries seven SDK
markers and six runtime build facts into its session.

What still depends on the app doing the work:

- **DET-10 needs React commit data.** The SDK has `reactCommit(...)` and the
  host accepts it, but a render count has to come from the app's own React
  profiling hook. Sampling a stack that happens to be inside React is not the
  same measurement.
- **DET-11 needs request spans.** `networkRequest(...)` exists and redacts
  query strings; the app has to call it from its own fetch layer.
- **G09, cross-runtime async.** Async span markers work; correlating spans
  across two JS runtimes needs a live app that actually has two.
- **No native iOS/Android SDK.** `sdk/ios` and `sdk/android` are empty: a
  native app with no JS layer has no way in yet.
- **Markers are the app's own account of itself**, on the app's clock, and the
  capability says so. Nothing in the platform corroborates them, and the host
  does not silently align the app's clock to the device's.

---

## 7. The clock-mapping path has one real producer, and it is coarse

`ClockMapping` carries a measured offset and its uncertainty, and
`NormalizedTrace::map_to_primary` **refuses to apply an unmeasured mapping**.
This is load-bearing: DET-02 can only reach `cause_status: candidate` when the
JS and UI clocks are mapped, so without a mapping it correctly declines to say
anything about UI impact.

A live Android capture now produces one measured mapping: the host steady
clock against the device's boot time, read once from `/proc/uptime`, with a
10 ms half-width and its method recorded. It exists to place `dumpsys meminfo`
readings, which carry no timestamp of their own -- it is not precise enough to
align a JS clock to a UI clock, and nothing uses it for that.

No collector produces a mapping between two *event* clocks, which is what
DET-02's UI-impact path needs. The fixtures include both cases (one measured,
one deliberately unmeasured) and both are tested.

---

## 8. Checksums detect corruption, not tampering

`session_store` uses FNV-1a 64 over file bytes. The manifest says so
explicitly (`checksum_purpose`). It catches truncation and accidental edits; it
is not a cryptographic integrity guarantee and must not be relied on as one.

---

## 9. Coverage of specification section 18

**108 of 198** checklist items have at least one automated test
(228 test cases in 12 binaries). The remaining 90 are enumerated with a stated
reason in `docs/requirement-test-map.md`; they cluster into: needs hardware,
needs a collector, needs the SDK, needs a UI.

A checklist item having a test is not the same as the capability being verified
on hardware. The capability matrix is the authority on that.

---

## 10. Things this tool deliberately does not do

Not limitations to be fixed -- design positions taken from spec sections 2.3,
0.12 and 0.26:

- No root, jailbreak, private API, or entitlement bypass, and none is offered
  as a fallback.
- No attempt to deeply profile an arbitrary App Store or Play Store
  application. Where that is impossible, the tool explains why rather than
  trying.
- No single opaque performance score.
- No numeric confidence value, because none of it is calibrated. Confidence is
  prose with a stated basis.
- No estimate of release performance from a debug measurement, by any route
  including subtraction.
- No root, and no attempt to lift Android's profiling restriction. `simpleperf`
  is invoked through `--app`, which works only on a debuggable or profileable
  package; anything else reports `permission_denied` with the manifest change
  that would fix it.
