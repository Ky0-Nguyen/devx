# Tested capability matrix

Generated from a real probe of this build host by `mpi preflight --json`.
Every row is a **measured probe result**, not a plan.

- Host: macOS 26.6.2, Apple M4 Pro, 48 GB RAM
- Android Platform Tools: adb 1.0.41 (36.0.0-13206524)
- Xcode 26.6 (17F113), devicectl 518.33, xctrace 16.0
- **Android emulator connected and booted** (`emulator-5554`, API 37,
  sdk_gphone16k_arm64, user build)
- iOS physical devices: two paired, both `unavailable` (tunnelState), both
  reporting `ddiServicesAvailable: false`
- iOS simulators: available, one booted (iPhone 17 Pro, iOS 26.5)

`tested` values mean:

| value | meaning |
|---|---|
| `verified_on_physical_device` | exercised against real hardware |
| `verified_on_simulator_or_emulator` | exercised, but only against a simulator or emulator |
| `probed_only` | the probe ran and answered; the capability itself was not exercised |
| `not_tested` | could not be probed here (usually: no suitable device) |

## Matrix

| Capability | Status | Provider | Tested |
|---|---|---|---|
| `android.toolchain.adb` | `available` | adb | `probed_only` |
| `android.discovery.devices` | `available` | adb | `verified_on_simulator_or_emulator` |
| `android.discovery.installed_apps` | `available` | adb shell pm | `verified_on_simulator_or_emulator` |
| `android.discovery.running_processes` | `available` | adb shell ps | `verified_on_simulator_or_emulator` |
| `android.discovery.process_mapping` | `available` | adb shell /proc | `verified_on_simulator_or_emulator` |
| `android.capture.profileable` | `unknown` | adb | `not_tested` |
| `ios.toolchain.xcrun` | `available` | xcrun | `probed_only` |
| `ios.discovery.devices` | `available` | devicectl + simctl | `verified_on_simulator_or_emulator` |
| `ios.discovery.installed_apps` | `unknown` | devicectl device info apps | `not_tested` |
| `ios.discovery.running_processes` | `unknown` | devicectl device info processes | `not_tested` |
| `ios.discovery.simulator_apps` | `available` | simctl | `verified_on_simulator_or_emulator` |
| `ios.capture.attach` | `unknown` | xctrace | `not_tested` |
| `ios.capture.live_recording` | `limited` | xctrace | `verified_on_simulator_or_emulator` |

| `ios.capture.log_store` | `permission_denied` | log | `probed_only` |

## Capture sources, verified on the Android emulator

These are reported per capture rather than at preflight, because whether a
source works depends on the selected target. Verified against the Shopper debug build (`com.acme.shopper.debug`) and, for the refusal case,
against `com.android.settings`:

| Source | Status on a debuggable target | Status on a non-debuggable target |
|---|---|---|
| `android.capture.frames` (`dumpsys gfxinfo framestats`) | `available` -- 78 frames at a platform-reported 60 Hz | `available`; the source does not depend on debuggability |
| `android.capture.cpu_samples` (`simpleperf`) | `available` -- 42 symbolised samples incl. React Native's `mqt_v_js` thread | `permission_denied`, with the manifest change that would fix it |
| `android.capture.memory` (`dumpsys meminfo`) | `available` -- five counter families | `available`; the reading is excluded from app-scoped totals when ownership is ambiguous, which is what a shared-uid system app produces |
| `android.capture.cpu_time` (`/proc/<pid>/stat`) | `available` -- 18 points over 6 ticks at CLK_TCK 100 | `available`; `/proc/<pid>/stat` is world-readable |
| app build identity (`dumpsys package`; reported as `app.*` build facts in preflight's `build` object, not as a capture source) | `available` -- version, install path, update time, signature digest, debuggable | `available`; the package's own flags line is read, not a permission's |
| `android.capture.streaming_overhead` (tick loop) | `available` -- 62 ticks, frames and memory per tick, CPU in background windows | `available` for frames and memory; CPU stays `permission_denied` |
| `android.capture.scheduling` (`atrace sched disk am view`) | `available`, **opt-in only** -- 4082 events over 64 threads in a 10 s capture | `available`; ftrace is system-wide and does not depend on the target's debuggability |
| `android.capture.heap_dump` (`am dumpheap`) | `available`, **opt-in only** -- 49 MB, 580,140 objects, read in 1.7 s | `permission_denied`: `am dumpheap` needs a debuggable target or a userdebug build |

| `android.capture.startup` (`am start -W`, after `cmd package resolve-activity --brief`) | `available` when a launcher activity resolves; `unsupported` with the reason when none does -- the component is taken from the platform, never guessed as `package/.MainActivity` | the same; `am start -W` does not depend on debuggability |

### Measured live, against a real emulator

Both verified on `emulator-5554` (Pixel 9 Pro image, API 37):

| Target | Ticks | Frames | Memory points | CPU samples | CPU coverage |
|---|---|---|---|---|---|
| `com.acme.shopper.debug` (debuggable) | 10 | 0 -- the app sat on a static screen and the platform's own counter also reported 0 | 60 | 1458 | 60.2%, 3 gaps between sampling windows |
| `com.android.settings` (not debuggable) | 62 | 53 while scrolling | 310 | 0 | 0%, gap reason `source_permission_denied` |

Neither row is a claim about phone hardware: an emulator's GPU is emulated and
its scheduler is the host's. Physical-device live capture is unverified.

### Capture behaviour under interference, measured on the same emulator

Four experiments, each against `com.acme.shopper.debug` on `emulator-5554`,
because a capture's honesty under interference cannot be argued from code:

| What was done | What happened |
|---|---|
| **The app force-stopped between listing and Record** (A17) | Listed as `running` with one process, then `am force-stop`; `mpi record` refused with "no live process ... could be resolved" rather than capturing against a dead pid |
| **The emulator killed 7 s into a 20 s live capture** (D20) | The session was written, `partial: true`, the reason naming the lost connection, and the 8 counter series collected before the loss kept. The window records 1.84 s -- what was actually measured, not the 20 s requested |
| **A CPU burner run in its own process throughout a capture** (I06) | 282 samples, **all attributed to exactly one process**; the burner appears nowhere in the app's numbers, and the app's own CPU time came from its own `/proc/<pid>/stat` at 2,790 ms over 11,268 ms |
| **`adb install -r` of the same APK** (B11) | The install path and `lastUpdateTime` changed while `versionName` and `versionCode` did not -- which is exactly what a developer rebuilding one version produces, and why build identity cannot rest on a version number |

Two of these found defects rather than confirming behaviour. The device-loss
case reported `partial: false` with no reason: the live loop ignored a tick's
outcome entirely, so a lost device looked like an app that went quiet. And the
reinstall case showed the Android build profile carried **no app build
identity at all** -- two facts about the device and nothing about the app --
so two captures either side of a reinstall were indistinguishable.

A third experiment is worth recording for what it did *not* show.
`adb kill-server` mid-capture changes nothing: the client respawns the server
and reconnects, the ticks keep succeeding, and nothing is marked partial. It
is not a disconnect, and an earlier attempt to test D20 with it proved
nothing. Killing the emulator is a disconnect.

And `KEYCODE_SLEEP` on an emulator turns the screen off without suspending the
shell or the app: through a 6 s screen-off the tick cadence never moved from
~710 ms and nothing was lost. That exercises a screen-off, not a suspend, so
D15 stays open.

### The collector changes the workload, measured with paired controls

Spec E18 and I20 ask for this and it is the least comfortable number here.
`com.acme.shopper.debug`, idle on `emulator-5554`, CPU read from its own
`/proc/<pid>/stat` over matched windows:

| Window | App CPU with no capture | With a live capture | Induced |
|---|---|---|---|
| 12 s | 650 ms | 2,430 ms | +1,780 ms |
| 10 s | 440 ms | 2,290 ms | +1,850 ms |
| 10 s | 450 ms | 2,010 ms | +1,560 ms |

**Why:** `dumpsys` is serviced by the target process. Asking the app for its
own memory or frame statistics makes the app do the work of answering, so
observing it costs it CPU -- and that cost lands inside the very process the
capture is measuring.

**It does not scale with the tick rate.** A 2,500 ms tick (four ticks in ten
seconds) induced 1,790 ms, about the same as a 700 ms tick (fourteen ticks).
So most of the cost is per-capture rather than per-tick, and slowing the
cadence does not buy much back.

**How to read it.** The app here is idle, using 4-6% of one core, so an extra
1.6-1.9 s is several times its own work -- a ratio that would be far smaller
on a busy app. The honest statement is the absolute figure: expect on the
order of 1.5-2 s of induced app CPU per capture on this emulator, and treat a
CPU measurement taken during a tick-based capture as including the cost of
being watched.

**It is not subtracted from anything**, and must not be: it is one app on one
emulator, and subtracting an estimate would turn a known perturbation into an
invented number. It is reported as a limitation on
`android.capture.streaming_overhead` instead, next to the host-side wall time
that figure used to report alone.

### CPU time against wall time, measured on the same emulator

The distinction spec E11 asks for, and the one every CPU finding is read as
though it already answered. A five-second live capture of
`com.acme.shopper.debug`: **790 ms of process CPU time over 5,107 ms of wall
time -- 15.5% of one core**, with user and system time reported separately
(272 s and 79 s cumulative since the process started).

`CLK_TCK` is read from the device rather than assumed. It is 100 on every
device seen, which is exactly what makes assuming it dangerous: on one where
it is not, every figure would be wrong by a constant factor and look
plausible. When it cannot be read the counter is not collected and the source
says so.

What the counter cannot say: it is whole-process, so it never identifies which
thread used the CPU; and the kernel counts in 10 ms ticks, so a difference
taken over a short interval is quantised to that and a busy millisecond can
read as zero.

### Heap evidence, measured on the same emulator

`mpi record --heap` against `com.acme.shopper.debug`: 580,140 objects,
292,343 roots (23,161 anchored in the app's own code), 31,349 classes, zero
unrecognised records. Android's 1.0.3 format is read directly -- `hprof-conv`
is not used, because it collapses root kinds and drops the heap attribution.
Objects split 246,284 app / 158,976 zygote / 139,839 boot image, which is the
split that decides what is the app's memory to release at all.

Two caveats, both stated to the operator before the dump is taken:

- **It pauses the app.** The runtime walks the whole heap, so the dump is
  taken after every other source and any timing measured across that point
  includes the pause.
- **It is one instant.** A dump shows what was reachable then. It says nothing
  about how long an object had been alive, and an object on a finalizer queue
  is legitimately present for one more collection cycle -- so presence is not
  retention.

### Scheduling evidence, measured on the same emulator

`mpi record --scheduling` against `com.acme.shopper.debug`: 4082 ftrace
events attributed across 64 of the app's threads, with
`sched_blocked_reason iowait=1 caller=folio_wait_bit_common` observed on the
React Native JS thread (`mqt_v_js`). The kernel reported no dropped events in
that capture (`entries-in-buffer` equalled `entries-written`); a capture where
they differ records the difference as a coverage gap rather than a quiet
period.

Two caveats specific to this source, and both matter more on an emulator than
elsewhere:

- **The scheduler is the host's.** An emulated device's context switches,
  block times and wake-ups are macOS scheduling decisions wearing Android's
  clothes. Durations measured here are not phone durations. This is the source
  whose numbers transfer *least* well off hardware.
- **It is off by default and says why.** `atrace` traces every process on the
  device, costs CPU at every context switch, and can overflow its ring buffer.
  The CLI states that before enabling it. A capture without `--scheduling`
  carries no scheduling evidence at all, and DET-03/DET-09 then report that
  the provider did not run.

## Booting a device, measured on this host

`mpi boot` and the Devices tab start a simulator or emulator and wait until it answers -- `sys.boot_completed` on Android, `Booted` from simctl -- reporting `started` and `ready` separately. By construction everything here is `verified_on_simulator_or_emulator`; no physical device is involved.

| What | Measured |
|---|---|
| cold AVD boot (API 37 image) | about 30 s; a second AVD started and never reported `sys.boot_completed` inside a 150 s budget, adb seeing it as `offline` throughout -- reported as `started: true, ready: false` with the serial and adb's last state |
| cold simulator boot | 1116 ms through `simctl boot`; an earlier run recorded 781 ms; re-booting one already running is a no-op that says so |
| serial of the emulator this boot produced | taken as the difference against the serials present beforehand, `null` when it cannot be confirmed |

**The budget was a lower bound on how long the wait could take.** Both loops checked the budget before each poll and then handed the poll its own fixed timeout: with the default 180 s budget and a 60 s per-call timeout, a poll starting at 179 s ran to 239 s, and the Android baseline `adb devices` ran its own 15 s outside the budget entirely. With a wedged `CoreSimulatorService` or adb server that bought three polls instead of ninety. Each call now gets whatever is left of the budget, capped, the sleep between polls is clamped to it, and a boot refuses to start when it cannot take a baseline -- an empty baseline would have reported an already-running emulator as the one it started. Three unanswered polls in a row stop the loop and blame the tooling, naming the fix, rather than reporting a device that never came up.

Pinned without hardware by `tests/helpers/hanging_tool.cpp`, a stand-in adb that either never answers or answers once and then wedges; 8 budget cases in `test_identity` (34/34 passing).

Not covered: whether a *listed* AVD will boot (`emulator -list-avds` reports names, not health), and two emulators started within one poll would be indistinguishable.

## Inspect sources, verified against a live app

`mpi inspect` reads a React Native debug build through the inspector it already connects to Metro; nothing is installed in the app. Its report carries a `SourceState` per source (`unavailable` / `attached` / `ran_saw_nothing` / `refused`) rather than this matrix's `tested` column, so what follows is what was exercised and against what. Everything was exercised against the Shopper debug build; nothing here was run against a physical device.

| What | Claims | Measured |
|---|---|---|
| `--redux` (state, read once) | store located by walking `__REACT_DEVTOOLS_GLOBAL_HOOK__`'s fiber tree to a prop carrying `getState`/`dispatch`/`subscribe`; read-only, bounded to 4000 fibers | 52 slices found 23 fibers in (Android emulator) |
| `--redux-watch` (state changes, read-only) | one `store.subscribe` listener, top-level keys compared by reference, removed at the end; names **no** action, because Redux passes subscribers none | a deep link produced a record with 30 deltas clearing a profile slice field by field |
| `--redux-actions` (action types and payloads) | wraps `store.dispatch` for the duration and restores it -- the one thing in this feature that **modifies the running app**, so it is opt-in and the report says whether removal was confirmed; a dispatch through a reference captured before the wrap (a thunk's) is recorded as `dispatch_bypassed`, not as a read-only record | a run kicked off its debugger slot left a wrapper behind; the next run reported it and removed it first |
| `--redux-values` | values off by default: a store holds tokens and personal data | -- |
| `--detail` (headers and bodies) | headers from the events, bodies via `Network.getResponseBody`; off by default because this is the data in flight | `getResponseBody` returned `cGFja2FnZXItc3RhdHVzOnJ1bm5pbmc=` (`base64Encoded: true`) -- `packager-status:running` |

The diff between two states is computed in C++ from the two JSON strings the app sends, bounded to 200 differences and 6 path segments, and hitting either bound is reported. A slice replaced by an equal value is reported as `equal_replacement` -- the classic wasted render -- and only when values were captured. 40 cases in `test_inspect` cover this, 19 of them the diff and attribution rules; all pass. The recorded fixture `fixtures/cdp/recorded-shopper-startup.jsonl` is a real session but carries no Redux drain (37 lines: `Runtime.consoleAPICalled`, `Network.*`, `Log.entryAdded`), so the live verification of the three Redux tiers rests on the record in `docs/inspect-without-installing.md` and commit 6f32c5e, not on a replayable fixture. The written procedure for driving the app during a watch uses `xcrun simctl openurl`, because Metro allows one debugger per device and a second socket takes the slot.

**Screenshots.** `--screenshot` requires `--device`. The id is translated to the display name discovery holds for it, because Metro publishes a device *name* and never a serial or UDID; passing the id through as the Metro hint made every screenshot run fail with "no attached device matches '456FA0D8-...'. What is attached: [iPhone 17 Pro]". `--target-device` still names a Metro target directly and wins when given. Android uses `adb exec-out screencap -p`; a simulator uses `xcrun simctl io <udid> screenshot`, and that path is selected only with `--platform ios` -- without it the UDID is handed to adb. A physical iOS device has no command-line screenshot and is reported unavailable rather than attempted. Output that is not a valid PNG is refused. What is not recorded anywhere in this repository is a successful post-fix simulator screenshot (dimensions, bytes): the fix's evidence is the failure it removed, and this row must not read as more than that.

## Capture sources, verified on the iOS simulator

A simulator app is an ordinary macOS process owned by the developer, so live capture there needs no Instruments: `SimulatorHostCollector` reads it through libproc. Verified against the booted iPhone 17 Pro (iOS 26.5) simulator `456FA0D8`, and reported per capture, like the Android sources above. Every row carries `tested: verified_on_simulator_or_emulator`; none of it says anything about a device.

| Source | Status | What it rests on |
|---|---|---|
| `ios.simulator.host_cpu_time` | `limited` -- CPU *time*, not attribution | `PROC_PIDTASKINFO`, converted from mach absolute time units (the per-thread counters are already nanoseconds; read as nanoseconds the task counters were 41.67x too small) |
| `ios.simulator.host_footprint` | `available` | `proc_pid_rusage` `ri_phys_footprint`, the value Xcode's memory gauge shows |
| `ios.simulator.frames` | `unsupported` -- tested and genuinely unavailable, which is a different answer from not tested | no command-line frame-timing source exists for an iOS simulator; the whole window is written as one coverage gap (`no_frame_source_for_ios_simulator`) so nothing here can support a frame-deadline finding |
| `ios.simulator.rosetta_translated` | `limited`, present only when `kinfo_proc` reports `P_TRANSLATED` | translated CPU timings are comparable neither to a native simulator build nor to a device |
| `ios.simulator.stack_profile` (`/usr/bin/sample`) | `limited`, **opt-in only** -- weighted stacks with no timestamps | `sample <pid> <seconds>` at the end of the capture, parsed into `CpuSample`s with self time split from time in callees |

Per-thread CPU times come with the runtime's own thread names (`com.facebook.react.runtime.JavaScript`, `hades`), so the JS-versus-native split on iOS is a platform signal rather than pattern matching.

**Stack attribution was measured after this document's sibling had stated it was impossible.** `task_for_pid` is refused (kr=5), and the conclusion drawn from that was wrong: `/usr/bin/sample` returned 1294 lines of symbolised call graph for the simulator's Calendar in under four seconds, with no root and no Full Disk Access. Through the collector, a live simulator capture produced 3 weighted stacks across 3 threads, heaviest weight 1735 at depth 13 with leaf `mach_msg2_trap`. The parser's conservation check -- self times summing exactly to what the threads declared -- holds at 37444 samples across 22 threads and on `fixtures/sample/recorded-simulator-callgraph.txt`.

What it cannot do is place any of it in time: `sample` reports an aggregate, so the source answers "where" and never "when", and the window is the seconds `sample` ran for at the end, not the capture's duration. It is off by default for that reason as much as for the cost (`sample` blocks, so the stop waits).

Two scoping facts. The stack profile can be switched on only from DevX's Live tab (a checkbox and a seconds stepper); `mpi record --live` never constructs this collector, so the CLI has no simulator live capture and no stack profile. And this collector refuses a batch `record` and refuses a physical device rather than degrading either.

## Evidence and limitations

### `android.toolchain.adb` -- Android Platform Tools (adb)

- **status**: `available`  
- **tested**: `probed_only`  
- **provider**: adb 1.0.41  
- **evidence**: `/Users/tan4194/Library/Android/sdk/platform-tools/adb --version` reported 1.0.41  

### `android.discovery.devices` -- Android device discovery

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`  
- **limitation**: every discovered Android device is an emulator; discovery is not verified against physical hardware    
- **provider**: adb 1.0.41  
- **evidence**: `adb devices -l` returned 1 device line(s)  
- **scope**: devices visible to this host's adb server; a device claimed by another adb server or an IDE may not appear  

### `android.discovery.installed_apps` -- Installed package enumeration

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`    
- **provider**: adb shell pm 1.0.41  
- **evidence**: `pm list packages -U` returned 260 package(s)  
- **scope**: packages visible to the shell user for the queried Android user  
- **limitation**: installed does not mean running, and does not mean profileable  

### `android.discovery.running_processes` -- Running process enumeration

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`    
- **provider**: adb shell ps 1.0.41  
- **evidence**: `ps -A -o PID,PPID,USER,NAME` returned 303 parsed row(s)  
- **scope**: processes visible to the shell user; shell has broader visibility than an ordinary app SDK would  

### `android.discovery.process_mapping` -- Process start-time / identity resolution

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`    
- **provider**: adb shell /proc 1.0.41  
- **evidence**: /proc/self/stat parsed; field 22 (starttime) is readable  
- **scope**: start time lets a process instance be distinguished across PID reuse and device reboot  

### `android.capture.profileable` -- Permission to profile a selected app

- **status**: `limited`    
- **tested**: `verified_on_simulator_or_emulator`    
- **provider**: adb 1.0.41  
- **evidence**: not determinable at the device level: it depends on the selected package's manifest (<profileable> / android:debuggable) and the device build type  
- **scope**: must be re-probed per selected package during preflight  
- **prerequisite**: the target APK declares <profileable android:shell="true"/> or is debuggable, or the device runs a userdebug/eng build  
- **recovery**: add <profileable android:shell="true"/> to the target's manifest and reinstall  

### `ios.toolchain.xcrun` -- Xcode command-line tools

- **status**: `available`  
- **tested**: `probed_only`  
- **provider**: xcrun  
- **evidence**: xcode-select -p => /Applications/Xcode.app/Contents/Developer; devicectl 518.33; xctrace version 16.0 (17F113)  

### `ios.capture.log_store` -- Unified log store access (Instruments sampling)

- **status**: `permission_denied`  
- **tested**: `probed_only`  
- **provider**: log  
- **evidence**: `log show --last 1s` was refused: log: Could not open local log store: Operation not permitted  
- **limitation**: xctrace reports this as "the log archive is corrupt or incomplete and cannot be read", which describes a damaged machine and is usually this permission. Every recording here came back with a time-profile table that had a schema and no rows.  
- **prerequisite**: Full Disk Access for whatever runs this tool, or running the capture from Xcode  
- **recovery**: System Settings > Privacy & Security > Full Disk Access, and add the terminal or app that runs this tool; or run the capture from Xcode, which already has it  

This is a host prerequisite, not a device one. It is probed at preflight so the refusal is found before a capture is attempted rather than after it comes back with an empty table.

### `ios.discovery.devices` -- iOS device discovery

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`  
- **provider**: devicectl + simctl 518.33  
- **evidence**: `devicectl list devices --json-output` reported 2 physical device(s) (0 reachable); `simctl list devices --json` reported 23 available simulator(s) (1 booted)  
- **scope**: devices paired with this host, plus local simulators. Simulators are reported separately and are never treated as physical devices.  
- **limitation**: verified against simulators only: no physical iOS device was reachable at probe time  
- **limitation**: 2 physical device(s) are paired but unreachable (tunnelState is not connected); they are reported as offline, not as absent  

### `ios.discovery.installed_apps` -- Installed app enumeration (physical device)

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: devicectl device info apps 518.33  
- **evidence**: every paired physical device is unreachable (connectionProperties.tunnelState != connected)  
- **prerequisite**: a reachable, paired physical device with Developer Mode enabled  
- **prerequisite**: Developer Disk Image services mounted (ddiServicesAvailable = true)  
- **recovery**: connect and unlock a trusted iPhone or iPad with Developer Mode enabled, then re-run discovery  

### `ios.discovery.running_processes` -- Running process enumeration (physical device)

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: devicectl device info processes 518.33  
- **evidence**: no reachable physical iOS device; not probed  
- **limitation**: iOS does not expose a general foreground/suspended signal, so even when this works the runtime state of an app is reported as running or unknown -- never guessed as suspended  
- **prerequisite**: a reachable physical device with DDI services  
- **recovery**: connect and unlock a trusted device  

### `ios.discovery.simulator_apps` -- Installed and running app enumeration (simulator)

- **status**: `available`  
- **tested**: `verified_on_simulator_or_emulator`  
- **provider**: simctl  
- **evidence**: simulator 456FA0D8-48C1-4BEC-B087-50E8A046EA5D: 29 app(s) listed, 3 with a launchd-attributed running process  
- **scope**: simulator only. Simulator timing is never comparable to a physical device and is kept in a separate baseline (spec J18).  
- **prerequisite**: a booted iOS simulator  
- **prerequisite**: `plutil` to convert simctl's NeXTSTEP plist output to JSON  

### `ios.capture.attach` -- Permission to attach a profiler to a selected app

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: xctrace xctrace version 16.0 (17F113)  
- **evidence**: not determinable at the device level: it depends on the selected app's code signature and entitlements. It is re-probed per app during preflight.  
- **scope**: an app the developer builds and signs. A third-party App Store app generally cannot be deeply attached, and this tool does not attempt to bypass that.  
- **limitation**: no jailbreak, private API, or entitlement bypass is used or offered  
- **prerequisite**: the app is signed with a development profile by this developer, or carries get-task-allow / the debugger entitlement  
- **prerequisite**: a reachable device with Developer Mode enabled  
- **recovery**: build and install the target from Xcode with a development signing identity, then retry  

### `ios.capture.live_recording` -- Live capture (physical device, and simulator)

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: xctrace xctrace version 16.0 (17F113)  
- **evidence**: a booted simulator is present and live capture works there without Instruments: the app is an ordinary host process, so CPU time and memory footprint are read through libproc. xctrace is not involved.    
- **limitation**: on a simulator this gives CPU *time* and memory, not frames. The frame part is a platform limit -- no command-line frame source exists for a simulator. Stacks are not: `/usr/bin/sample` profiles a simulator app, and the collector ingests its call graph when the stack profile is switched on (`ios.simulator.stack_profile`, below)  
- **limitation**: simulator timings are not device timings, and an app running translated under Rosetta is not comparable to a native build either -- the capture records which  
- **limitation**: UNVERIFIED on a physical device: no physical iOS device has been reachable, so that path is implemented and not demonstrated. Instruments cannot record a *simulator* target at all on this host -- it accepts the target and never starts recording -- so a working simulator answer does not transfer to hardware    
- **prerequisite**: a reachable physical device, OR a booted simulator for the host-process collector  
- **prerequisite**: an attachable, developer-signed app  
- **recovery**: for a device, connect a trusted physical iPhone or iPad and re-run `mpi preflight`; the simulator path is already usable    

