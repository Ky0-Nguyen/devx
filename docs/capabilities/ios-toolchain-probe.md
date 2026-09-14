# iOS toolchain probe record (M0)

Specification section 3.4 requires the installed Xcode tooling to be probed and
its advertised commands and output schemas **validated locally** rather than
assumed, and section 20 requires discovery commands and JSON formats to be
confirmed during M0. This file is that record.

Probed on 2026-09-14 on macOS 26.6.2 / Apple M4 Pro.

## Toolchain versions

| Tool | Version | How obtained |
|---|---|---|
| Xcode | 26.6 (17F113) | `xcodebuild -version` |
| Developer dir | `/Applications/Xcode.app/Contents/Developer` | `xcode-select -p` |
| `devicectl` | 518.33 | `xcrun devicectl --version` |
| `xctrace` | 16.0 (17F113) | `xcrun xctrace version` |

Raw output is stored under `fixtures/provider-output/*.real.*`.

## Finding 1 -- `devicectl` JSON output to a file is the only supported machine interface

`xcrun devicectl device info --help` states, verbatim in its own help text,
that JSON output to a user-provided file on disk is the ONLY supported
interface for scripts and programs to consume command output.

**Consequence for the implementation.** `IosAdapter::run_devicectl_json`
always passes `--json-output <tempfile>` and parses that file. The adapter
never parses devicectl's stdout. The temp file is created with `mkstemp` and
unlinked by an RAII guard.

## Finding 2 -- `devicectl list devices` schema, confirmed

Top level is `{"info": ..., "result": {"devices": [...]}}`. The fields the
adapter reads, each confirmed present in real output:

| Path | Observed value on this host | Used for |
|---|---|---|
| `identifier` | `3FF46431-775C-59BB-AD26-D316DFAFA5A6` | the stable `device_id`; it is the handle other devicectl commands accept |
| `hardwareProperties.udid` | `00008101-000978CA11A1001E` | fallback id only |
| `hardwareProperties.reality` | `physical` | `DeviceForm` -- never assumed |
| `hardwareProperties.platform` | `iOS` | platform filter |
| `hardwareProperties.marketingName` | `iPhone 12 Pro Max` | model |
| `deviceProperties.name` | `QuocBao's iPhone` | display name |
| `deviceProperties.osVersionNumber` | `26.0` | OS version |
| `deviceProperties.osBuildUpdate` | `23A340` | OS build |
| `deviceProperties.developerModeStatus` | `enabled` | a distinct blocker from pairing |
| `deviceProperties.ddiServicesAvailable` | `false` | **gates app and process enumeration** |
| `connectionProperties.pairingState` | `paired` | trust |
| `connectionProperties.tunnelState` | `unavailable` | reachability |
| `connectionProperties.transportType` | absent while offline | wired vs network |

## Finding 3 -- paired is not reachable, and neither is authorized

Both devices on this host reported `pairingState: paired` with
`tunnelState: unavailable`. The adapter maps that to
`TrustState::kOffline`, so `usable_for_capture()` is false.

This matters because a naive reading of `pairingState` alone would have
presented both devices as ready and produced a capture attempt that cannot
work. Asserted in `test_ios_parsers.cpp::unreachable_paired_device_is_offline_not_authorized`.

## Finding 4 -- `ddiServicesAvailable: false` is the real gate on a physical device

Both devices report `false`. The Developer Disk Image services are what back
`devicectl device info apps` and `devicectl device info processes`. The adapter
reads this field and, when it is false, reports
`CapabilityStatus::kPermissionDenied` with the recovery action "open Xcode with
the device connected so it prepares the developer disk image" -- rather than
reporting an empty app list.

## Finding 5 -- `simctl listapps` emits a NeXTSTEP plist, not JSON

`xcrun simctl listapps <udid>` output begins:

```
{
    "com.apple.Bridge" =     {
        ApplicationType = System;
        CFBundleIdentifier = "com.apple.Bridge";
```

That is an old-style property list. `simctl list devices --json` *is* JSON, but
`listapps` has no `--json` flag on this version.

**Consequence.** The adapter writes the plist to a temp file and converts it
with `plutil -convert json -r -o <out> <in>`, then parses the result. Both
subprocesses are argv-only; there is no shell pipeline. Confirmed working
against 27 apps on the booted simulator.

## Finding 6 -- launchd labels give real ownership evidence on a simulator

`xcrun simctl spawn <udid> launchctl list` is tab-separated `PID Status Label`,
and app processes appear as:

```
12833	0	UIKitApplication:com.apple.mobilecal[079d][rb-legacy]
12473	0	UIKitApplication:com.apple.Spotlight[42d5][rb-legacy]
```

The label embeds the bundle id. This is **launchd itself attributing the
process to the bundle**, which is why the adapter records these as
`OwnershipEvidence::kProviderAttributed` -- the strongest tier -- rather than
as a name match.

A `-` in the PID column means the job is loaded but not running, and is
correctly reported as not running rather than as a live process.

Caveat recorded in the ownership note: `launchctl list` gives no process start
time, so PID reuse cannot be excluded from this source alone.

## Finding 7 -- no foreground or suspended signal

Nothing in `devicectl device info processes` or `launchctl list` exposes
foreground or suspended state. Per spec A09 the adapter therefore never emits
`RuntimeState::kSuspended`, and annotates running entries with
"foreground/background state is not observed by this provider".

## What remains unvalidated

`devicectl device info apps` and `devicectl device info processes` have
**never been run against a reachable device** on this host, because none was
reachable. Their parsers are written against Apple's documented field names and
tested against hand-constructed documents, and both capabilities are recorded
as `not_tested` in the capability matrix. Their schemas must be re-confirmed
against real output before either capability is claimed.
