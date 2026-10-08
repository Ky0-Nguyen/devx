# ADR-0013 -- External agents, internal intelligence

- Status: accepted
- Date: 2026-10-08
- Spec references: Intelligence spec sections 3.2, 16

## Context

Shipping a model would mean paying for inference per user, choosing one vendor, and making answers the source of truth.

## Decision

DevX owns evidence, retrieval and deterministic computation; the reasoning comes from the agent the user already has (Claude, Codex, Cursor) through `mpi mcp`. DevX computes percentiles, joins, failure locations in logs and code context itself, and hands over a bounded, cited, redacted evidence pack (`intelligence_evidence_pack`, at most 64 KB). An AI answer is never the only place a problem is shown: every fact cites a local evidence id.

## Consequences

- No model, embedding index or vector database in DevX.
- Redaction (`core/signals/redact.cpp`) runs on every excerpt and pack before it leaves the store; raw evidence stays local, with a warning where it is shown.
