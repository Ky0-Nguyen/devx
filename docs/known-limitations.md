# Known limitations

This is the register the specification asks for. It is written to be read by
someone deciding whether to trust a result, so it states what is **not** true
of this build as plainly as what is.

Nothing here is a surprise discovered at the end. Each entry has a phase and,
where one exists, a concrete remediation.

---

## 1. Live on-device capture is not implemented (blocks the M2 gate on both platforms)

**What this means.** `mpi record` does not record. There is no Perfetto
collector, no `xctrace` collector, and no session controller driving one. The
command performs discovery, target pinning, capability preflight and
revalidation, and then either imports a trace you supply or exits
`unsupported` (6) with the blocker stated.

**Why it is not disguised.** Spec section 0.16 forbids substituting import-only
support for the required live workflow, and section 0.5 forbids presenting a
dashboard over synthetic data as a working profiler. Writing a session package
with no collector output would produce a capture-shaped file containing nothing
measured, so the command refuses instead.

**Consequence.** The M2 gate -- *connect -> select identifier -> record ->
issue on one physical device of each platform* -- **is not met**. Checklist
items J14 and J15 are open.

**Phase.** M2.

---

## 2. No physical device was reachable, so the live path is UNVERIFIED

**Android.** No Android device was connected at any point during
implementation. `adb` itself is present (1.0.41) and its host-side output is
parsed and tested against real output, but `pm list packages`, `ps -A`,
`/proc/<pid>/stat` and `dumpsys` were **never run against hardware**. Their
parsers are tested against hand-written fixtures named `.synthetic.`, and the
capability matrix records every one of those capabilities as `not_tested`.

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

## 3. The 1 GiB stress fixture is processed, but with a 10x memory blowup

**Measured on this host** (macOS 26.6.2, Apple M4 Pro, 48 GB RAM), with a
1.0 GiB normalized trace containing 2,920,000 events:

| | |
|---|---|
| wall time, ingest + normalize + analyze + JSON export | **12.5 s** |
| peak resident set size | **10.2 GB** |
| events ingested | 2,920,000 (all of them) |
| exit code | 4 (`inconclusive`) -- correctly, since this fixture has no frames, JS spans or samples, so every detector was skipped |

**The problem.** Roughly 10 bytes of RAM per byte of input. `json::Value` is a
DOM node carrying a `std::string` and two `std::vector`s regardless of which
variant it holds, and the whole document is materialised before normalization
begins. On a 16 GB machine a 1 GiB trace would likely fail.

**Also note.** The default input ceiling was originally 512 MiB, which *refused*
the specification's own 1 GiB stress fixture. It is now 2 GiB, overridable with
`mpi analyze --max-input-mib <n>`. The refusal was correct and clearly reported;
the default was simply wrong.

**Remediation (M2).** Replace the DOM parse on the ingest path with a streaming
pull parser that emits `model::Event` values directly into a bounded queue. The
`Reader` interface already hides the parse strategy, so this is contained to
`core/ingestion` and needs no model or rule changes.

**Related checklist.** I21 (UI responsiveness under the stress fixture) cannot
be assessed without a UI. The ingest cost above is the part that is measurable
today.

---

## 4. Eight of the twelve catalog detectors are registered but not implemented

Implemented: **DET-01** (frame deadlines), **DET-02** (long JS), **DET-04**
(sampled CPU hotspot), **DET-12** (tooling attribution).

Registered and always skipped, each with its prerequisites, its phase and the
conclusion it will be allowed to reach: DET-03 (sync main-thread I/O), DET-05
(memory growth), DET-06 (retention), DET-07 (startup budget), DET-08
(regression), DET-09 (lock contention), DET-10 (React renders), DET-11 (network
delay).

**Why register them at all.** Spec H05 requires an unsupported rule to be
*reported as skipped* and H11 requires "no findings" to be distinguishable from
"no analysis". A detector the engine has never heard of can satisfy neither.
Every report therefore lists all twelve with an explicit outcome.

**Note on DET-08.** The comparison *engine* is implemented and tested
(24 cases, `mpi compare`). The detector is still marked M4 because a defensible
regression verdict needs the repeated-run scenario workflow, which does not
exist yet.

---

## 5. No desktop UI

M1 ships a CLI only. See ADR-0004 for why, and for the seam the UI will sit on.
Spec section 13's views are all renderings of `DiscoverySnapshot`,
`NormalizedTrace` and `AnalysisResult`, which already serialize to JSON and are
already exercised by the CLI.

Checklist items that are inherently UI behaviours (A16, A23, A25, I21, J12)
are open.

---

## 6. No app SDK, so no markers, no build handshake, and no React data

`sdk/android`, `sdk/ios` and `sdk/react-native` are empty directories. Without
an SDK:

- **Screen and interaction names are never known.** `Issue::screen` serializes
  to `null` unless a marker covered the interval. The tool does not guess a
  screen from a function name (spec section 11).
- **Build facts come only from the device and the host.** The
  build-plugin-manifest and runtime-SDK fact sources exist in the model and are
  honoured by `BuildProfile::upsert`, but nothing populates them, so most facts
  are `unknown` and benchmark eligibility is usually `insufficient_evidence`.
- **React render analysis is impossible.** DET-10 requires React profiler
  commit data. Deriving renders from Hermes CPU samples would misreport them, so
  it is skipped rather than approximated.

**Phase.** M3.

---

## 7. The clock-mapping path is implemented but has no real producer

`ClockMapping` carries a measured offset and its uncertainty, and
`NormalizedTrace::map_to_primary` **refuses to apply an unmeasured mapping**.
This is load-bearing: DET-02 can only reach `cause_status: candidate` when the
JS and UI clocks are mapped, so without a mapping it correctly declines to say
anything about UI impact.

No collector currently produces a measured mapping. The fixtures include both
cases (one measured, one deliberately unmeasured) and both are tested.

---

## 8. Checksums detect corruption, not tampering

`session_store` uses FNV-1a 64 over file bytes. The manifest says so
explicitly (`checksum_purpose`). It catches truncation and accidental edits; it
is not a cryptographic integrity guarantee and must not be relied on as one.

---

## 9. Coverage of specification section 18

**108 of 198** checklist items have at least one automated test
(226 test cases in 12 binaries). The remaining 90 are enumerated with a stated
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
