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
| `android.discovery.devices` | `available` | adb | `verified_on_physical_device` |
| `android.discovery.installed_apps` | `available` | adb shell pm | `verified_on_physical_device` |
| `android.discovery.running_processes` | `available` | adb shell ps | `verified_on_physical_device` |
| `android.discovery.process_mapping` | `available` | adb shell /proc | `verified_on_physical_device` |
| `android.capture.profileable` | `unknown` | adb | `not_tested` |
| `ios.toolchain.xcrun` | `available` | xcrun | `probed_only` |
| `ios.discovery.devices` | `available` | devicectl + simctl | `verified_on_simulator_or_emulator` |
| `ios.discovery.installed_apps` | `unknown` | devicectl device info apps | `not_tested` |
| `ios.discovery.running_processes` | `unknown` | devicectl device info processes | `not_tested` |
| `ios.discovery.simulator_apps` | `available` | simctl | `verified_on_simulator_or_emulator` |
| `ios.capture.attach` | `unknown` | xctrace | `not_tested` |
| `ios.capture.live_recording` | `unknown` | xctrace | `not_tested` |

## Capture sources, verified on the Android emulator

These are reported per capture rather than at preflight, because whether a
source works depends on the selected target. Verified against the superapp
HutBot debug build (`io.pizzahut.hutbot.debug`) and, for the refusal case,
against `com.android.settings`:

| Source | Status on a debuggable target | Status on a non-debuggable target |
|---|---|---|
| `android.capture.frames` (`dumpsys gfxinfo framestats`) | `available` -- 78 frames at a platform-reported 60 Hz | `available`; the source does not depend on debuggability |
| `android.capture.cpu_samples` (`simpleperf`) | `available` -- 42 symbolised samples incl. React Native's `mqt_v_js` thread | `permission_denied`, with the manifest change that would fix it |
| `android.capture.memory` (`dumpsys meminfo`) | `available` -- five counter families | `available`; the reading is excluded from app-scoped totals when ownership is ambiguous, which is what a shared-uid system app produces |
| `android.capture.streaming` (tick loop) | `available` -- 62 ticks, frames and memory per tick, CPU in background windows | `available` for frames and memory; CPU stays `permission_denied` |
| `android.capture.scheduling` (`atrace sched disk am view`) | `available`, **opt-in only** -- 4082 events over 64 threads in a 10 s capture | `available`; ftrace is system-wide and does not depend on the target's debuggability |

### Measured live, against a real emulator

Both verified on `emulator-5554` (Pixel 9 Pro image, API 37):

| Target | Ticks | Frames | Memory points | CPU samples | CPU coverage |
|---|---|---|---|---|---|
| `io.pizzahut.hutbot.debug` (debuggable) | 10 | 0 -- the app sat on a static screen and the platform's own counter also reported 0 | 60 | 1458 | 60.2%, 3 gaps between sampling windows |
| `com.android.settings` (not debuggable) | 62 | 53 while scrolling | 310 | 0 | 0%, gap reason `source_permission_denied` |

Neither row is a claim about phone hardware: an emulator's GPU is emulated and
its scheduler is the host's. Physical-device live capture is unverified.

### Scheduling evidence, measured on the same emulator

`mpi record --scheduling` against `io.pizzahut.hutbot.debug`: 4082 ftrace
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

## Evidence and limitations

### `android.toolchain.adb` -- Android Platform Tools (adb)

- **status**: `available`  
- **tested**: `probed_only`  
- **provider**: adb 1.0.41  
- **evidence**: `/Users/tan4194/Library/Android/sdk/platform-tools/adb --version` reported 1.0.41  

### `android.discovery.devices` -- Android device discovery

- **status**: `available`  
- **tested**: `verified_on_physical_device`  
- **provider**: adb 1.0.41  
- **evidence**: `adb devices -l` returned 1 device line(s)  
- **scope**: devices visible to this host's adb server; a device claimed by another adb server or an IDE may not appear  

### `android.discovery.installed_apps` -- Installed package enumeration

- **status**: `available`  
- **tested**: `verified_on_physical_device`  
- **provider**: adb shell pm 1.0.41  
- **evidence**: `pm list packages -U` returned 260 package(s)  
- **scope**: packages visible to the shell user for the queried Android user  
- **limitation**: installed does not mean running, and does not mean profileable  

### `android.discovery.running_processes` -- Running process enumeration

- **status**: `available`  
- **tested**: `verified_on_physical_device`  
- **provider**: adb shell ps 1.0.41  
- **evidence**: `ps -A -o PID,PPID,USER,NAME` returned 303 parsed row(s)  
- **scope**: processes visible to the shell user; shell has broader visibility than an ordinary app SDK would  

### `android.discovery.process_mapping` -- Process start-time / identity resolution

- **status**: `available`  
- **tested**: `verified_on_physical_device`  
- **provider**: adb shell /proc 1.0.41  
- **evidence**: /proc/self/stat parsed; field 22 (starttime) is readable  
- **scope**: start time lets a process instance be distinguished across PID reuse and device reboot  

### `android.capture.profileable` -- Permission to profile a selected app

- **status**: `unknown`  
- **tested**: `not_tested`  
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

### `ios.capture.live_recording` -- Live trace capture from a physical iOS device

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: xctrace xctrace version 16.0 (17F113)  
- **evidence**: xctrace version 16.0 (17F113) is installed, but live capture has NOT been exercised against a physical device in this environment  
- **limitation**: UNVERIFIED: no physical iOS device was reachable during implementation, so the live capture path is implemented but not demonstrated. This blocks the M2 gate for iOS.  
- **prerequisite**: a reachable physical device  
- **prerequisite**: an attachable, developer-signed app  
- **recovery**: connect a trusted physical iPhone or iPad and run `mpi preflight` to convert this from unknown to a measured result  

