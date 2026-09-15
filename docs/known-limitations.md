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

### Observing the app costs the app CPU, and the figure is now measured

`dumpsys` is serviced by the target process, so polling it makes the app do
the work of answering -- inside the very process being measured. Paired
control runs on the emulator: an idle app used 440-650 ms of CPU over a
10-12 s window with no capture and 2.0-2.4 s with one, so roughly 1.5-2 s of
induced work per capture. It did not scale with the tick rate (four ticks cost
about as much as fourteen), so most of it is per-capture.

On an app that idle this is several times its own work; on a busy app the
ratio would be far smaller, which is why the absolute figure is the honest
one to quote. Nothing is subtracted -- one app on one emulator is not a
correction factor, and subtracting an estimate would turn a known
perturbation into an invented number. It is stated as a limitation on the
overhead source, which previously reported only the host's wall time.

### CPU time is collected now, so "busy" and "waiting" are separable

The process's own user and kernel CPU time, read from `/proc/<pid>/stat` and
sampled on the same tick as memory. It is the counter that separates a process
that was *busy* from one that was *waiting*, which wall-clock time cannot do
and every CPU finding gets read as though it could. Measured on the emulator:
790 ms of CPU over 5,107 ms of wall time.

`CLK_TCK` is read from the device, never assumed. It is 100 everywhere seen,
which is what makes assuming it dangerous -- a wrong constant would scale
every figure and look plausible. When it cannot be read the counter is not
collected and the source says why.

Whole-process only: it never says which thread used the CPU. And the kernel's
10 ms tick means a difference over a short interval is quantised, so a busy
millisecond can read as zero.

### What the Android collector does not collect

**No network timing.** DET-11 reads network markers from the app's own SDK,
so a capture without an instrumented app reaches no network conclusion at all.
Nothing on the device side is read for it: `dumpsys netstats` counts bytes per
uid, which is not a request timeline.

**Scheduling is opt-in, not on by default.** `--scheduling` runs
`atrace sched disk am view`, which is a system-wide kernel trace: it costs CPU
on every context switch across every process, and its ring buffer can overflow
and drop events. The CLI states that before enabling it, and a capture without
the flag carries no scheduling evidence -- DET-03 and DET-09 then skip and say
the provider was not run, which is not the same as finding nothing.

An earlier version of this file said scheduling data required taking on
Perfetto's protobuf. That was wrong, and the correction matters because it was
the stated reason two detectors stayed unimplemented: `atrace` without `-z`
prints plain ftrace text, which carries `sched_switch` with `prev_state`,
`sched_blocked_reason` with the kernel's `iowait` flag, and `sched_waking`.
That is exactly the evidence DET-03 and DET-09 need, with no new dependency.
Perfetto would still buy a better-bounded buffer and per-syscall detail;
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

**I21, now measured rather than deferred.** DevX was driven against a real
1 GiB session (2,920,000 events, a 1.8 GB package on disk), and the answer
splits in two:

- **Opening it is cheap: the window rendered in about 1 second at 111 MB of
  resident memory.** That is by design rather than luck -- the Issues view
  reads the session's `report.json`, which is bounded by the number of
  findings, not by the capture. The 1 GiB raw trace is never loaded to show
  it.
- **The Timeline tab was not cheap: 7.8 GB of peak resident memory**, settling
  at 2.9 GB, because binning re-reads the whole trace. It rendered correctly
  and the app survived on this 48 GB machine. On a 16 GB machine it would have
  ended the process.

So the timeline now refuses a trace over **256 MiB** by default and says what
it measured: the trace's size, the 7.8 GB figure, and that
`mpi timeline --max-input-mib` is how to say the machine has the room. A
measurement that would take the process down is not a default worth having,
and a silent 8 GB allocation is not a considered one either.

For comparison, the same fixture on the command line: `mpi analyze` takes 8.0
s and 5.5 GB; `mpi timeline` takes 7.4 s and 5.1 GB and emits a **170 KB**
document. That last number is the design holding: whatever the capture's size,
what the UI has to render is bounded by the bin count.

---

## 4. All twelve catalog detectors are implemented

**DET-01** (frame deadlines), **DET-02** (long JS), **DET-03** (synchronous
main-thread I/O), **DET-04** (sampled CPU hotspot), **DET-05** (memory growth
across screen cycles), **DET-06** (retained-object investigation), **DET-07**
(startup budget), **DET-08** (regression), **DET-09** (wait contention),
**DET-10** (React renders), **DET-11** (network delay), **DET-12** (tooling
attribution).

Implemented is not the same as *runnable on any capture*, and the difference
is the point of the run record. A detector whose provider was not collected
skips and names the evidence it wanted -- never a milestone, which a test now
enforces across all twelve.

**Implemented does not mean exercised on every platform.** DET-03 and DET-09
read Android scheduling evidence only; on an iOS capture they skip for want of
the provider. DET-10 and DET-11 read the app SDK, so they skip on any capture
from an app that did not handshake. The capability matrix is the authority on
what has been exercised against what.

**Why register them at all.** Spec H05 requires an unsupported rule to be
*reported as skipped* and H11 requires "no findings" to be distinguishable from
"no analysis". A detector the engine has never heard of can satisfy neither.
Every report therefore lists all twelve with an explicit outcome.

**DET-06 never says "leak" either, and the reason is sharper.** Its evidence
is a heap dump, which proves two things exactly: that an object existed at one
instant, and what chain of references reached it from a GC root. Both are read
from the file. Neither is a leak -- a cache, a singleton, an object pool and a
framework-held instance are all reachable by design.

So the rule reports what it can check: an object whose **own lifecycle state**
says it is finished, still held by a chain. `mDestroyed` on an Activity is the
framework's statement about itself, not this tool's opinion, which is why the
threshold origin is `platform_contract` -- a category added for it, because
calling a documented contract a "configurable heuristic" would have been a
lie about where the number came from. The finding names the field it read,
because a framework-private field is a version-dependent thing to depend on.

Three refusals are built in. An object no root reaches is reported as
**garbage awaiting collection**, which is the opposite of retention. A chain
anchored only in runtime bookkeeping -- an interned string, a VM internal --
says the *runtime* holds the object and the finding says so rather than
blaming the app. And **retained size is not computed at all**: it needs a
dominator tree, and an approximation presented as a size would be a number
nobody could check. Shallow size is reported, named as shallow, with a note
that an Activity holding a 40 MB bitmap has a shallow size of a few hundred
bytes.

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

**DET-03 will not promote a blocked thread to an I/O finding.** A thread in
uninterruptible sleep (`D`) is blocked on *something*; only the kernel's own
`sched_blocked_reason` with `iowait=1` says that something was I/O. Without
that flag the rule records the state and reports nothing, because "the main
thread stalled" and "the main thread stalled on disk" are different claims and
this provider distinguishes them. What it still cannot supply is the app's own
call stack: ftrace gives the kernel function that blocked
(`folio_wait_bit_common`, say) and the app's `Trace.beginSection` slice if one
was open, which together locate the block without proving which line of app
code caused it. That gap is on every finding. It also keeps the three "main"
threads apart -- the UI main thread, the JS thread, and the native-module
threads -- because a blocking read means something different on each.

**DET-09 names the waker, never a lock owner.** `sched_waking` records which
thread made another runnable. That is not the same as which thread held a
lock: a thread can be woken by a timer, a binder reply, or an unrelated
signal, and the kernel does not report ownership. So the wait duration is
measured and the cause stays qualified, the finding names the waking thread as
the waker only, and it says that identifying a lock and its owner needs
instrumentation this build does not have. It reports only user-visible threads
-- a background worker blocking is its job -- and lists the threads it passed
over, and it hands I/O waits to DET-03 instead of counting one stall twice.

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

`DevX.app` is a native SwiftUI application over the C ABI (ADR-0007), and it
now covers **every view specification section 13 lists**: Devices, Apps,
Preflight, Record and Live, Sessions, Issues, Timeline, Compare, Detectors and
Export/settings. Two of the eleven are panels rather than tabs, which is worth
stating rather than leaving to be inferred -- the issue detail and the
stack/source view live inside Issues, next to the finding they describe.

Two views beyond that list. **Threads** shows the JS-versus-native split,
which matters because "the main thread" means a different thread depending on
which one you mean: a React Native app has a UI thread that draws, a JS thread
running the app's own code, and native-module threads between them, and a
blocking read means something different on each. The view leads with how the
split was decided, because that is its weakest part -- on Android the JS
thread is identified by its *name* (`mqt_v_js`, `mqt_js`, anything containing
`hermes`), since no platform signal says which thread runs JavaScript. Every
row carries its basis, and only the UI main thread gets to claim a platform
signal. Measured on a real capture: UI main 34%, JS 17%, renderer 0.4%, and
48.6% on threads no rule could name -- reported as `other` rather than
guessed into a role.

**Start a simulator or emulator** is on the Devices tab, and `mpi boot` is the
CLI form. It keeps "started" and "ready" apart, which is the whole point: see
section 11.

The Live tab streams a capture alongside the running app: counters, memory
sparklines, per-source status, and the preliminary banner over everything
until the window closes.

The Export view carries the one statement section 13 asks for that has no
control attached: "no source or trace upload without configured user consent".
There is no consent switch, because there is nothing to consent to -- this
build has no upload path. Exporting writes a local file, and the view says so
outright rather than offering a toggle that implies an upload exists.

Still open, and all inherently UI behaviours:

- Suppressions are now a project file both front ends read
  (`suppressions.json` beside the sessions directory), with a mandatory
  reason, an author, an optional reference and an expiry that is honoured.
  Building it found a defect worth naming: **the engine recorded a
  suppression's expiry and never checked it**, so a suppression written with a
  2024 expiry was still hiding findings in 2026. An expiry that never expires
  is worse than no expiry, because it creates the belief that suppressions
  lapse. An expired one is now reported and not applied, and an unparseable
  one keeps the suppression while saying the date could not be read -- the
  other way round would silently un-suppress on a typo.
- **A25** responsiveness with a very large app list. The emulator's 265 apps
  render fine; nothing larger has been tried.
- Recents and favourites exist now, and spec A23 turned out to be an honesty
  rule rather than a feature: a remembered entry is not proof the app is
  running. No such row carries a runtime state. What it shows is where the
  entry stands against the *current* enumeration, and an entry absent from
  that listing says "not in the current listing" -- never "not running",
  because a listing that could not see an app and an app that is gone are
  different facts. A third state covers having no listing to compare
  against.
- **I21** responsiveness under the 1 GiB stress fixture.
- **J12** signing and packaging for distribution. Documented now in
  `docs/packaging-and-signing.md`: the bundle is ad-hoc signed (`--sign -`,
  a signature with nobody behind it), which runs where it was built and
  nowhere else. The five steps a distributable build needs are listed; none
  has been done, because there is no Developer ID in this environment. The
  license inventory is empty and that is the state to preserve.
- **A16** keyboard focus across a list refresh stays unverified, and the
  reason is worth naming: driving keyboard focus needs macOS Accessibility
  permission, which this environment does not grant to the automation, so a
  fix could not be *checked* even if written. Selection is preserved and
  tested; focus is not claimed.
- **The timeline exists.** DevX has a Timeline tab over a binned view whose
  bins carry a state rather than a bare number, so an unmeasured stretch
  cannot be drawn as zero: four states, four visual treatments, and the
  legend above the tracks rather than below them. `mpi timeline` renders the
  same data in a terminal. An issue focuses its own interval at full
  resolution, and `--issue=<id>` opens a link to one finding. Building it
  found three defects in the capture path, listed in section 12.
- **The compare view exists.** DevX has a Compare tab over the same engine
  the CLI uses, and the run-set reader moved into the core rather than being
  written twice -- what an absent field means is part of the comparison's
  honesty, not a parsing detail. Whether a pair may gate a release is stated
  *before* the verdict, and `inconclusive` says in words that it is not "no
  change".
- **A file path the app was handed is probed before it is read.** macOS gates
  Documents, Desktop and Downloads behind a consent prompt that an ad-hoc
  signed build cannot raise, and the block happens inside the read: a run-set
  path typed into the field or passed as `--baseline` hung the operation
  behind a spinner that never ended. The probe has a deadline and a third
  answer -- readable, unreadable, or *did not return* -- and the last one is
  reported as a permission wall by name. Files chosen through the open panel
  are unaffected: picking a file is what grants access to it.
- The Live tab has no frame-timeline track either; it shows counts, source
  status and memory series. The Timeline tab reads a written session, so it
  cannot be pointed at a capture still in progress.
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

### There is no native SDK, and that is a scope decision

`sdk/ios` and `sdk/android` contain a README each and nothing else. The
specification calls the app SDK *optional* and defines its job as reporting
what no device provider can see -- which screen mounted, which navigation was
cancelled, which interaction the user started, what build the bundle came
from. For a React Native app every one of those facts lives in JavaScript, and
`sdk/react-native` reports all of them with no native code.

What a native SDK would add is stated in each directory's README rather than
implied by an empty folder. The one that would improve an existing detector:
an Android SDK emitting `Trace.beginSection` would put the app's own slices
into an `atrace` capture, which DET-03 already reads when they are there.

### The sample app is a module that runs and components that do not

`samples/react-native` holds a two-screen app. Its `instrumentation.js` --
the module a real app imports, where every SDK call lives -- is executed
against a live host by `samples/react-native/verify.mjs`, 23 checks, wired
into `tools/smoke-test.sh`. `App.js` is **not built**: it needs React Native,
npm, Gradle and Xcode, none of which runs here. Its own README leads with that
distinction, because a sample that looks tested and is not is worse than no
sample.

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

**177 of 198** checklist items have at least one automated test
(457 test cases in 19 binaries, plus 250 Swift, both harnesses declaring the checklist ids they cover). The remaining 21 are enumerated with a stated
reason in `docs/requirement-test-map.md`; they cluster into: needs hardware,
needs an iOS recording that completes, needs a UI test harness.

A checklist item having a test is not the same as the capability being verified
on hardware. The capability matrix is the authority on that.

Twenty-three items have moved from uncovered to covered without new
capability being built, because their stated reasons were wrong rather than
merely stale. Two mistakes ran through them. Several said "needs a device"
while an emulator was running and a real React Native app was installed on
it. More importantly, several confused *confirmation* with the requirement:
the checklist asks whether **this tool** distinguishes, refuses or keeps
things apart, and that is decided by its own logic -- hardware would confirm
the behaviour, a test establishes it. A14 (two apps sharing a display name),
B14 (profileable independent of running and visible), A09 (no foreground
state to guess at), F09 (no cross-process memory sums) and J19 (no fabricated
JS evidence) are all of that kind.

An earlier batch of twelve moved for the plainer reason that their stated
reason had gone stale: six said "needs a
collector (M2)" when the collector had existed for some time. What they
actually needed was a collector to *drive*, not a device -- so they are
driven against a fake one, which is a test double rather than synthetic data
dressed up as a measurement. One of them found a real defect: `LiveSession`
silently stopped a running capture when a second `start()` arrived, losing
the capture in flight and tearing down the device-side collectors without
anyone asking. A second capture is refused now, with the refusal noted on the
running session's own snapshot where the operator is looking.

---

## 10. The heap parser is verified against one real dump, which is not committed

`mpi record --heap` runs `am dumpheap` and stores the HPROF file in the
session. The parser reads Android's own 1.0.3 format directly rather than
converting it with `hprof-conv`, because that conversion discards the two
things a retention question needs: it collapses nearly every root to
`ROOT_UNKNOWN` (269,081 of 290,000 in the dump below), and it drops the
`HEAP_DUMP_INFO` records that separate the app's heap from the zygote and boot
image.

**What was measured.** One dump, 49 MB, from `io.pizzahut.hutbot.debug` on
`emulator-5554`: 580,140 objects, 292,343 roots of which 23,161 are anchored
in the app's own code, 31,349 classes, zero unrecognised records, three
dangling references, and 246,284 objects on the app heap against 158,976 on
the zygote heap and 139,839 on the boot image. Read in 1.7 s. It found one
real retention chain -- a JNI global holding `ReactHostImpl`, which holds
`MainActivity` as `defaultHardwareBackBtnHandler`.

**The dump is not in the repository**, because 49 MB of one app's heap does
not belong in one. The parser's tests build HPROF bytes in the test file
instead, which is how the cases worth testing exist at all: a class described
after its own instances, a truncated file, an undefined field type. Those are
labelled synthetic in the test's own header.

**Two bugs the real dump caught**, both of which the synthetic tests now pin:

- **A single pass loses references silently.** HPROF does not guarantee a
  class appears before instances of its subclasses, and in this dump it did
  not -- `MainActivity`'s instance record preceded
  `androidx.activity.ComponentActivity`'s class record. The chain walk stopped
  at the first class it had not read, so 37 of that object's 59 references
  were never read: not corrupted, just missing, which is the harder failure to
  notice. The parser now reads every class first.
- **A first-pass artifact reported as data loss.** With two passes, the
  first pass was still walking field blocks and counting every not-yet-read
  class as an incomplete chain -- 190,433 "losses" on a dump that had none.
  Reporting a loss that did not happen is the same class of error as hiding
  one that did.

**What is unverified.** No physical device, and only one app. `mDestroyed`
existing on `android.app.Activity` is checked on API 37 only; on a platform
version where that field is renamed or means something else, DET-06 skips for
want of a lifecycle state rather than reporting something wrong. The positive
path -- a destroyed object still held -- has never been produced on a real
device: the app under test does not retain its Activity across a rotation, so
DET-06 correctly reported nothing, and the firing path is covered by the
built-in-bytes tests alone.

---

## 11. Starting a device is not the same as having one

`mpi boot` and the Devices tab's panel start a simulator or emulator. Two
distinctions are load-bearing, and both were measured rather than assumed.

**An AVD name is not a device id.** `emulator -avd Pixel_9_Pro` produces
something adb calls `emulator-5554` -- unless something is already on that
port, in which case it is `emulator-5556`. So a boot reports the device id it
*observed* afterwards, taken as the difference against the serials present
before, and reports `null` rather than guessing when it could not confirm one.

**"Started" is not "ready".** Measured on this machine: a cold AVD came up in
about 30 s, and a second one started and never reported `sys.boot_completed`
inside a 150 s budget -- adb saw it as `offline` the whole time. That is
reported as `started: true, ready: false` with the serial that appeared and
the state adb last saw, because a capture taken against that device would have
measured the boot. An iOS simulator booted in 781 ms and re-booting one
already running is a no-op that says so.

**What booting does not change.** A booted emulator is still an emulator:
nothing here touches the device form, and every measurement taken on it stays
labelled that way. Timings from it are never comparable to a physical device,
which is the same rule the device list states at the top.

**Not covered.** Whether a *listed* AVD will actually boot -- the listing
comes from `emulator -list-avds`, which reports names, not health. And the
Android path assumes the new serial is the one this boot produced; two
emulators started at once within the same poll would be indistinguishable.

---

## 12. Sessions recorded before 2026-09-15 have two coverage defects

Building the timeline meant reading coverage per source for the first time,
and it surfaced two faults in captures written before that date. Both are
fixed; both are still true of session packages already on disk.

**A batch capture wrote coverage rows only for sources that failed.** The
streaming path had per-source rows precisely because a source with no row is
indistinguishable from a source that measured nothing (H11); the batch path
never got them. So in an older batch session, frames and memory have no
coverage row, and nothing downstream can tell "ran and measured nothing" from
"never ran". The row builder is now one function called from both paths, and
`mpi timeline` on such a session says "no collector recorded a coverage window
for this source" rather than drawing zeroes.

**A coverage row could declare a zero-length window.** In some older sessions
the simpleperf row has `window_start == window_end` while its gaps carry
absolute timestamps from the real window. A row like that establishes no
coverage at all. The timeline reports it as a defect in the capture -- in those
words -- rather than as a quiet period, because the two would otherwise look
identical.

Neither is repairable after the fact: the coverage a collector did not record
cannot be reconstructed from the events it kept. Re-record if a session's
coverage matters.

---

## 13. Things this tool deliberately does not do

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
