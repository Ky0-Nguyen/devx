# ADR-0010 -- A provider-neutral connector contract

- Status: accepted
- Date: 2026-10-08
- Spec references: Intelligence spec sections 12, 13

## Context

Each provider added as bespoke code would bring its own UI, store and AI tool, and every new one would touch every consumer.

## Decision

All integrations implement `SignalConnector` (`core/signals/connector.hpp`): `info`, `capabilities`, `validate`, `discover`, `sync`. A connector writes only through `SignalSink`, the persistence boundary, and does its I/O through an injected `Transport`, so tests replay recorded replies through the code that parses live ones. Core names no provider: adapters register theirs (`adapters/intelligence/builtin.cpp`). A connector declares its credential's environment variable and Keychain service; the credential is resolved at sync time and held in memory only. Settings that look like secrets are refused when saved.

## Consequences

- Sentry, GitLab CI, Firebase (BigQuery exports) and JSONL ship in 0.5.0; the JSONL connector and a test connector registered from outside core prove a provider needs no consumer change (AC-12).
- Provider timestamps, states and payloads are kept as given; normalization is deterministic and explainable from the raw evidence.
