# ADR-0006 -- Android capture uses the platform's text interfaces, not Perfetto

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 2.2, 5, 8; checklist E01, F08, J14

## Context

Spec section 2.2 proposes the Perfetto C++ Trace Processor "where format and
provider support is verified". Perfetto is present on the device
(`/system/bin/perfetto`, `traced` running) and is the platform's flagship
tracing system.

Its output is protobuf.

## Decision

Android capture is built on three text interfaces instead:

| Source | Yields |
|---|---|
| `dumpsys gfxinfo PKG framestats` | one record per frame, with a platform-supplied deadline |
| `simpleperf record` + `report-sample --show-callchain` | symbolised stacks, thread names, and build facts |
| `dumpsys meminfo PKG` | memory counters, per family |

## Why

**Protobuf would end the empty licence inventory.** Consuming Perfetto's
output needs a protobuf runtime, or a hand-written protobuf parser for a
schema we do not control. ADR-0002 exists to keep the inventory empty, and
these three sources already supply the data the implemented detectors need.

**`framestats` is better evidence, not a worse substitute.** It reports
`FrameDeadline` per frame -- the deadline the platform itself applied. That is
strictly stronger than a deadline derived from an assumed refresh rate, and
`FrameInterval` gives the observed rate for free. DET-01 therefore never has
to guess, which is the whole point of spec section 8's "no universal 16 ms
rule".

**simpleperf's `meta_info` carries build facts read off the device.**
`app_type: debuggable`, `android_build_type`, `android_sdk_version`. These
are `FactSource::kDeviceProvider` observations rather than host-side
inferences, which is exactly what the build-detection model in spec section 7
wants.

## Consequences

- Verified against a real Android emulator (API 37) on the superapp HutBot
  debug build: 42 symbolised samples including React Native's `mqt_v_js`
  thread, 78 frames at a platform-reported 60 Hz, five memory families.
- `simpleperf --app` routes through `run-as`, so it works only on a debuggable
  or profileable package. On anything else the CPU source reports
  `permission_denied` with the manifest change that would fix it. Android's
  restriction is not worked around, per spec section 0.12.
- `framestats` **does not drain on read**. It is a ring buffer of roughly the
  last 120 frames, returned in full on every read, so consecutive reads
  overlap heavily. This ADR claimed the opposite until streaming proved
  otherwise: appending each read's rows wholesale recorded 523 frames across 8
  ticks where about 120 had actually rendered, a five-fold inflation. Frames
  are now identified by `IntendedVsync`, which is unique and monotonic per
  frame, so a read is idempotent. The collector still resets the history at
  capture start so the window holds only this capture's frames, and
  `--no-frame-reset` keeps what the platform already has.
- A ring buffer also means frames can be **lost between reads**: if more than
  its capacity rendered since the last tick, the oldest are gone. A read that
  is entirely new rows and full to capacity says exactly that, so the stretch
  it lost is recorded as a coverage gap -- missing evidence, not an idle app --
  and the fix is a shorter `--tick-ms`.
- No scheduling, I/O or network data. Those detectors stay registered and
  skipped rather than being approximated from these sources.

## When to revisit

If a detector needs scheduling states (DET-09), off-CPU time, or
cross-process flow events, these sources cannot supply them and Perfetto
becomes the right answer. At that point the protobuf dependency should be
weighed deliberately and recorded here -- not smuggled in.
