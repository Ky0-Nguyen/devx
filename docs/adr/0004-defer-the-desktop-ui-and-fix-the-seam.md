# ADR-0004 -- Defer the desktop UI to M2 and fix its seam now

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 2.2, 13, 17 (M1/M2 gates)

## Context

The specification names a desktop application as a confirmed requirement and
proposes Qt 6/QML as the implementation default. Qt was not installed on the
build host, is a large dependency, and carries a redistribution-license review
that section 5 explicitly calls out.

M1's gate is the C++ core and offline analysis. M2's gate is the live desktop
workflow.

## Decision

M1 ships **no GUI**. Instead the seam the GUI will sit on is defined and
exercised now, by the CLI:

- `discovery::DiscoveryService` already returns everything the picker needs:
  per-entry runtime state, visibility scope, profiling availability, freshness
  timestamps, provider errors, and `apply_filter` with the
  never-hide-unavailable-entries rule.
- `DiscoveryService::revalidate` already implements the
  refresh-without-retargeting behaviour spec A16/A17 require, and reports
  process-set changes as notes rather than acting on them.
- Every view spec section 13 lists reads from `AnalysisResult`,
  `NormalizedTrace` or `DiscoverySnapshot`, all of which serialize to JSON.

The GUI is therefore a rendering layer over an existing, tested API rather than
a rewrite.

## Consequences

- The M2 gate (connect -> select identifier -> record -> issue) is **not met**,
  and is recorded as open in `docs/known-limitations.md`.
- The framework choice stays open. Because the seam is a data API and not a
  callback surface, Qt/QML, or a web shell over the CLI's JSON, are both still
  viable without touching the core.
- The CLI is a first-class deliverable in its own right, which the
  specification also requires ("reusable headless CLI").
