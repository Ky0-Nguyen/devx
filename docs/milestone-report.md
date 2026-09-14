# Milestone report -- M0 and M1

Reported in the format specification section 21 requires: what works with real
evidence, how it was tested on each platform, what is unsupported or
unverified, which checklist ids are done or deferred, and the next concrete
step.

Host: macOS 26.6.2, Apple M4 Pro, 48 GB RAM. Date: 2026-09-14.

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
