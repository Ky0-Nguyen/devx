# ADR-0008 -- A separate signal plane

- Status: accepted
- Date: 2026-10-08
- Spec references: section 10; Intelligence spec sections 3.3, 8, 10

## Context

DevX measures: a session is a window of time on one device, and `NormalizedTrace` is its contract, with clocks, coverage and the rule that unknown is not zero (ADR-0005). Intelligence brings evidence of another shape: a Sentry issue, a GitLab job, a Firebase metric. It lives for weeks, belongs to a release rather than a window, and has no clock domain or coverage. Forcing it into `NormalizedTrace` would either weaken the trace's semantics or invent coverage nobody measured.

## Decision

Production, CI/CD, release, testing and external observability evidence uses its own contract, `SignalRecord` (`devx.signal/1`, `core/signals/signal.hpp`), with `ReleaseIdentity` and an `EvidenceBasis` on every record. `NormalizedTrace` stays the contract for measurement sessions only. The two planes meet only in correlation (`core/correlation`), which reads both and changes neither.

## Consequences

- Session packages, the rules engine, Timeline and Compare are untouched by Intelligence.
- A signal can be absent from a release (no identity) without anything being invented for it.
- Two stores exist side by side under the sessions directory: `s-*` session packages and `intelligence/<workspace>/`.
