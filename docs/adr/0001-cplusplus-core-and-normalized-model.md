# ADR-0001 -- A C++20 core over one normalized model, with platform adapters outside it

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 0.4, 2.2, 5, 6

## Context

The specification requires ingestion, normalization, analysis, attribution and
report generation to live in C++, and requires both iOS and Android as product
requirements. Android's Perfetto-based tooling and Apple's `devicectl` /
`xctrace` tooling have almost nothing in common at the transport or format
level.

Section 2.2 is explicit that we must not invent one universal trace collector.

## Decision

Three layers, with a hard boundary between them:

1. **Platform adapters** (`adapters/android`, `adapters/ios`) own everything
   platform-specific: process invocation, output schemas, capability probing,
   identity resolution. They depend on the core; the core never depends on them.
2. **One normalized model** (`core/model`) that every adapter targets. Anything
   an analysis rule needs to know about provenance, coverage, clocks, ownership
   or build state travels inside `NormalizedTrace`.
3. **A platform-agnostic analysis engine** (`core/rules`) that reads only the
   normalized model and never queries a device.

## Consequences

- A rule is written once and is honest on both platforms, because it reasons
  over evidence rather than over a provider.
- Adding a platform means writing an adapter, not touching the rules.
- The normalized model becomes the compatibility surface, so it is versioned
  (`schema_version`) and every field that can be unknown is optional rather
  than defaulted.
- `analyze` works with no device and no UI, which is what made offline
  development possible while no hardware was reachable.

## Rejected alternatives

- **A shared collector abstraction.** Would have forced a lowest-common
  denominator and produced metrics whose meaning differed by platform while
  sharing a name -- exactly what spec E22 forbids.
- **Analysis inside each adapter.** Would have duplicated every rule and
  guaranteed the two platforms drifted apart.
