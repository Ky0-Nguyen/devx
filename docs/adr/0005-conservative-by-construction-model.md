# ADR-0005 -- Honesty is enforced by types, not by convention

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 0.7-0.10, 6, 7.1, 8, 9, 10.1; checklist C18, E13,
  F14, H11, H16, H17

## Context

Most of this specification is a list of claims the tool must not make:
unknown is not false, a missing sample is not idle time, correlation is not
causation, debug minus overhead is not release, "no issue found" is not "the
detector did not run".

A convention ("remember to check for unknown") fails the first time someone
adds a rule. These had to become properties of the model.

## Decision

Each prohibition is encoded where it cannot be bypassed:

| Claim the spec forbids | How the code prevents it |
|---|---|
| unknown rendered as false | `model::Tri` has three states; `BuildProfile::boolean` returns `kUnknown` for an absent fact; `BuildFact::to_json` emits `null` |
| a missing metric read as zero | `Metric::value` is `std::optional<double>`; JSON emits `null` and a separate `measured` flag |
| a missing sample read as idle | `Coverage`/`CoverageGap` are first-class; a collector with no events yields a full-window gap, not silence |
| a proxy presented as truth | `FrameSource` distinguishes presentation timestamps from a display-callback proxy; `is_presentation_truth` is serialized; DET-01 cannot return `observed` from a proxy |
| correlation presented as cause | `DetectionStatus` and `CauseStatus` are separate fields; DET-02 can reach at most `kCandidate` |
| release estimated by subtraction | `AttributionReport` always carries `original_total` and serializes `subtraction_to_estimate_release_permitted: false` |
| inclusive shares summed as costs | `CandidateStack::inclusive` drives `summable_as_disjoint_cost` |
| "found nothing" conflated with "did not run" | `RuleOutcome` has three values; every registered rule emits a `RuleRunRecord` even when skipped |
| an uncalibrated confidence number | there is no numeric confidence field; `confidence_basis` is prose, and a test asserts no `confidence` key exists |
| a guessed screen name | `Issue::screen` serializes to `null` unless a marker covered the interval |
| a benchmark pass from unknowns | `evaluate_eligibility` returns `kInsufficientEvidence` for any unknown that could affect timing, and `certified_benchmark()` is false under a user override |

## Consequences

- A new rule inherits the guarantees. It cannot emit a bare number, and it
  cannot report "no issue" without also declaring that it ran.
- The 12 detectors in the catalog are all *registered*, including the 8 not yet
  implemented, because H05 and H11 can only be satisfied by a rule the engine
  knows exists. Each declares its prerequisites, its phase, and the conclusion
  it will be allowed to reach.
- Reports are longer and more hedged than a typical profiler's. That is the
  intended trade: section 0.24 requires missing data to stay explicit.
