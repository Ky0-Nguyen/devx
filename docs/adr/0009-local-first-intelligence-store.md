# ADR-0009 -- A local-first Intelligence store

- Status: accepted
- Date: 2026-10-08
- Spec references: Intelligence spec sections 3.3 (Invariant A), 11, 19

## Context

Provider APIs are slow, rate limited, need credentials, and change. An investigation that re-fetches everything every time cannot run offline, cannot be audited later, and makes AI tools call vendor APIs with the user's tokens.

## Decision

Every connector writes what it found to the local store **before** the UI, correlation or an AI tool treats it as evidence: normalized records under `signals/<date>/`, the provider's bytes under `raw/` (immutable: different bytes under the same name get a new version), a cursor, and a sync status that distinguishes complete, partial and failed. Writes are atomic (temporary file, then rename); directories are 0700 and files 0600. The index is a cache that is rebuilt from the records whenever a sync did not finish cleanly. A corrupt record is quarantined and named, never silently dropped. JSON and JSONL, no database (ADR-0002); a database is a later decision on measured evidence.

## Consequences

- Everything Intelligence shows works offline once synced, and says how fresh it is.
- A partial sync never makes the cache look complete: the watermark moves only on a complete sync.
- Retention is explicit (7, 30, 90 days or forever, pins override), and disconnecting a provider deletes nothing local unless asked.
