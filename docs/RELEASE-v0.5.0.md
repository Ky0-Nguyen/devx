# DevX v0.5.0

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.5.0.dmg`. Signed with a Developer ID and notarized.
Homebrew offers it once the release's pull request (version and cask) is
merged into `main`.

## What changed

- **Intelligence.** A new top-level module, DevX > **Intelligence**, puts
  production and CI/CD evidence next to your captures. It has seven views:
  Overview, Releases, Production, CI/CD, Signals, AI Analysis and
  Integrations. See `docs/intelligence.md`.
  - **Connectors:**
    - **Sentry:** issues, crashes, latest events, releases with their commits.
    - **GitLab CI:** pipelines, jobs, tests and deployments. The failing line
      of a failed job's log is found locally, and the log is kept.
    - **Firebase:** Crashlytics and Performance Monitoring from BigQuery export
      files, with percentiles computed by DevX.
    - **JSONL:** any tool's evidence.
  - **Local first.** A sync stores normalized records plus the provider's raw
    evidence before anything reads them, so everything works offline and says
    how fresh it is. A partial sync says partial and resumes, and the cache
    never looks more current than it is.
  - **Releases.** Evidence is grouped by release identity. The same full
    commit, or the same app + version + build, is labelled exact; a provider's
    own statement is attributed; timing or partial metadata is only a
    candidate, shown apart, and never called a cause. Conflicting sources are
    shown, not resolved. DevX sessions join releases through the app version
    their capture recorded.
  - **Compare two releases:** crashes, CI failures, affected users (only where
    reported) and metric percentile deltas.
  - **Code context** from your git repository: the release's commit, stack
    frames resolved to files, the lines around them, blame (by name, never
    e-mail) and the release's diff.
  - **AI tools.** `mpi mcp` gains ten Intelligence tools, including
    `intelligence_evidence_pack`. The pack is a bounded (64 KB), cited and
    redacted set of facts for your agent to reason over. Provider tokens
    stay in the Keychain or the environment and never reach a model.
  - **Retention** of 7, 30 or 90 days, or forever, with pins. Disconnecting a
    provider keeps its evidence unless you ask to delete it.
  - `mpi intelligence` does all of it from a terminal.
- **`mpi mcp --allow-network`.** A third permission tier: refreshing evidence
  from a provider (`connector_sync`) is separate from `--allow-actions`. With
  no flags, an agent reads local evidence only.
- **What leaves this Mac** is now an egress ledger (Settings,
  Intelligence > Integrations, `mpi intelligence egress`), connector by
  connector. It replaces the old "nothing leaves this machine", which stopped
  being true with BrowserStack and connectors.
- ADR-0008 to ADR-0013 record the design: a separate signal plane, a
  local-first store, a provider-neutral connector contract, three-tier MCP
  permissions, exact-before-candidate correlation, and external agents with
  internal intelligence.

## Known limits

- GitLab CI was exercised live, anonymously on a public project. Sentry and
  Firebase are tested against fixtures in their documented API and export
  shapes, not against live accounts.
- Without a commit and build in what CI and the crash reporter send, joins are
  candidates. That is visible, never hidden.
- Redaction knows credential shapes, not meaning; raw views warn.
- The desktop app pulls when you sync; it receives no webhooks.
