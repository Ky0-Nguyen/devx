# ADR-0012 -- Exact before candidate correlation

- Status: accepted
- Date: 2026-10-08
- Spec references: Intelligence spec section 14; ADR-0005

## Context

Joining a crash to a release, a release to a pipeline, and a pipeline to a local session is the expensive part of an investigation, and it is where tools overstate: a crash that started after a deploy is not caused by it.

## Decision

Correlation (`core/correlation/engine.cpp`) joins by deterministic identity first: the same full commit SHA, or the same app + version + build. A provider's own statement is `provider_attributed`. A short SHA prefix, a version without its build, or timing alone is `candidate`, stored apart from exact links and never called a cause. A signal with an exact link gets no candidate link to another release. When sources disagree about a release (two commits for one build), the conflict is kept and shown. A user's own link between a session and a release is recorded as theirs.

## Consequences

- The UI and the AI tools render exact and candidate links differently, and an evidence pack keeps them in separate lists.
- Without build provenance, joins degrade to candidates, visibly; reporting a commit and build from CI is what makes them exact.
