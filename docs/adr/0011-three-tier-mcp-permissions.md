# ADR-0011 -- Three-tier MCP permissions

- Status: accepted
- Date: 2026-10-08
- Spec references: Intelligence spec section 16.1

## Context

`mpi mcp` had two tiers: read-only by default, and `--allow-actions` for tools that change something (record, boot, operate a device, upload). Refreshing Intelligence evidence is neither: it reaches an external provider with the user's credentials and changes the local cache, but it operates nothing.

## Decision

A third effect, `Effect::kNetwork`, gated by `--allow-network`, separate from `--allow-actions`. By default an AI tool reads local sessions, observations, signals, releases, correlations and code context. `connector_sync` needs `--allow-network`. Mutations stay under `--allow-actions`. Neither flag implies the other.

## Consequences

- An agent can analyse a release offline with no flags at all.
- Existing tools keep their names and effects; the new tier cannot block a local read.
- MCP output never contains a provider credential, because none is ever stored.
