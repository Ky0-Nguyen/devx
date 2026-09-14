# Tested capability matrix

Generated from a real probe of this build host on 2026-09-14 by
`mpi preflight --json`. Every row is a **measured probe result**, not a plan.

- Host: macOS 26.6.2, Apple M4 Pro, 48 GB RAM
- Android Platform Tools: adb 1.0.41 (36.0.0-13206524)
- Xcode 26.6 (17F113), devicectl 518.33, xctrace 16.0
- Android device connected at probe time: **none**
- iOS physical devices: two paired, both `unavailable` (tunnelState)
- iOS simulators: available, one booted (iPhone 17 Pro, iOS 26.5)

`tested` values mean:

| value | meaning |
|---|---|
| `verified_on_physical_device` | exercised against real hardware |
| `verified_on_simulator_or_emulator` | exercised, but only against a simulator |
| `probed_only` | the probe ran and answered; the capability itself was not exercised |
| `not_tested` | could not be probed here (usually: no suitable device) |

## Matrix

| Capability | Status | Provider | Tested |
|---|---|---|---|
| `android.toolchain.adb` | `available` | adb | `probed_only` |
| `android.discovery.devices` | `available` | adb | `probed_only` |
| `android.discovery.installed_apps` | `unknown` | adb | `not_tested` |
| `android.discovery.running_processes` | `unknown` | adb | `not_tested` |
| `android.discovery.process_mapping` | `unknown` | adb | `not_tested` |
| `android.capture.profileable` | `unknown` | adb | `not_tested` |
| `ios.toolchain.xcrun` | `available` | xcrun | `probed_only` |
| `ios.discovery.devices` | `available` | devicectl + simctl | `verified_on_simulator_or_emulator` |
| `ios.discovery.installed_apps` | `unknown` | devicectl device info apps | `not_tested` |
| `ios.discovery.running_processes` | `unknown` | devicectl device info processes | `not_tested` |
| `ios.discovery.simulator_apps` | `available` | simctl | `verified_on_simulator_or_emulator` |
| `ios.capture.attach` | `unknown` | xctrace | `not_tested` |
| `ios.capture.live_recording` | `unknown` | xctrace | `not_tested` |

## Evidence and limitations

### `android.toolchain.adb` -- Android Platform Tools (adb)

- **status**: `available`  
- **tested**: `probed_only`  
- **provider**: adb 1.0.41  
- **evidence**: `/Users/tan4194/Library/Android/sdk/platform-tools/adb --version` reported 1.0.41  

### `android.discovery.devices` -- Android device discovery

- **status**: `available`  
- **tested**: `probed_only`  
- **provider**: adb 1.0.41  
- **evidence**: `adb devices -l` returned 0 device line(s)  
- **scope**: devices visible to this host's adb server; a device claimed by another adb server or an IDE may not appear  
- **limitation**: no device was connected at probe time, so device discovery is probed but not verified against hardware  

### `android.discovery.installed_apps` -- android.discovery.installed_apps

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: adb 1.0.41  
- **evidence**: no Android device was connected, so this could not be probed  
- **prerequisite**: an authorized Android device connected over USB or TCP  
- **recovery**: connect an Android device with USB debugging enabled  

### `android.discovery.running_processes` -- android.discovery.running_processes

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: adb 1.0.41  
- **evidence**: no Android device was connected, so this could not be probed  
- **prerequisite**: an authorized Android device connected over USB or TCP  
- **recovery**: connect an Android device with USB debugging enabled  

### `android.discovery.process_mapping` -- android.discovery.process_mapping

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: adb 1.0.41  
- **evidence**: no Android device was connected, so this could not be probed  
- **prerequisite**: an authorized Android device connected over USB or TCP  
- **recovery**: connect an Android device with USB debugging enabled  

### `android.capture.profileable` -- android.capture.profileable

- **status**: `unknown`  
- **tested**: `not_tested`  
- **provider**: adb 1.0.41  
- **evidence**: no Android device was connected, so this could not be probed  
- **prerequisite**: an authorized Android device connected over USB or TCP  
- **recovery**: connect an Android device with USB debugging enabled  

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

