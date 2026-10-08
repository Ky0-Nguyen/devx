# Intelligence: production and CI/CD evidence, local and correlated

DevX > **Intelligence** puts production crashes, CI pipelines, deployments and
production metrics next to the measurements DevX already takes. It groups all
of them around the release they belong to, and hands an AI tool a small, cited
evidence pack instead of a dashboard to read.

```bash
mpi intelligence workspace create superapp --repo ~/src/superapp --apps com.acme.app --env production
mpi intelligence connect sentry sentry-prod --setting organization=acme --setting project=mobile
mpi intelligence connect gitlab gitlab-ci --setting project=acme/superapp
mpi intelligence sync                       # every connector that is not paused
mpi intelligence releases
mpi intelligence release com.acme.app@5.4.0+54019
mpi intelligence compare com.acme.app@5.3.0+53000 com.acme.app@5.4.0+54019
mpi intelligence pack --release com.acme.app@5.4.0+54019 --question "why did 5.4.0 regress?"
```

The design is in [ADR-0008 to ADR-0013](architecture.md#architecture-decision-records).
In short:

- **Local first.** A sync writes everything to this Mac before anything reads
  it: normalized records, plus the provider's raw evidence. Every view, the CLI
  and every AI tool then work offline, and say how fresh the evidence is.
- **One model for every provider.** Sentry, GitLab, Firebase and JSONL imports
  all become `devx.signal/1` records and share one Signals view and one set of
  AI tools. A new provider changes none of them.
- **Exact before candidate.** Evidence is joined by identity (the same commit,
  or the same app + version + build) before anything else. Timing alone is a
  candidate, shown apart, and never called a cause.
- **AI tools get evidence, not credentials.** Tokens stay in the Keychain or
  the environment. `mpi mcp` returns bounded, redacted, cited facts.

## Workspaces

A workspace is a project: a repository root (for code context), the app ids
built from it (to pick out DevX sessions), environments, and a retention
policy. The window has a form for it under Intelligence > Integrations;
`mpi intelligence workspace create` does the same. Ids are lowercase letters,
digits, `-` and `_`.

Everything a workspace holds lives under
`<sessions dir>/intelligence/<workspace>/`, owner-only:

```
workspace.json  connectors/<id>.json  cursors/<id>.json
signals/index.jsonl  signals/<YYYY-MM-DD>/<signal-id>.json
raw/<provider>/<category>/<name>      quarantine/  state/  links/
```

## Connectors

| provider | what it reads | credential | leaves this Mac |
|---|---|---|---|
| **Sentry** (`sentry`) | issues and crashes with their latest event (release, environment, stack frames), and releases with their commits | `SENTRY_AUTH_TOKEN`, or the Keychain item `com.devx.sentry` (org:read, project:read, event:read) | authenticated API requests to your Sentry |
| **GitLab CI** (`gitlab`) | pipelines, jobs, the log of each failed job with its root error located, test report summaries, deployments | `GITLAB_TOKEN`, or the Keychain item `com.devx.gitlab` (read_api); **optional for public projects** | API requests to your GitLab |
| **Firebase** (`firebase`) | Crashlytics and Performance Monitoring rows from **BigQuery export files** you produced (`bq query --format=json`, `bq extract --destination_format=NEWLINE_DELIMITED_JSON`) | none | nothing: it reads files |
| **JSONL import** (`jsonl`) | any tool's evidence as one JSON object per line | none | nothing |

Settings are listed by `mpi intelligence providers` and shown as fields in the
window. A setting whose name looks like a secret (`token`, `password`, `dsn`,
...) is refused: credentials belong in the Keychain or the environment, never
in a file.

**Getting a token.** Wherever a connector needs one, the window has a
*get a token ↗* button that opens the provider's own page, and
`mpi intelligence connect` and `integrations` print the same link:

- Sentry: `<base_url>/settings/account/api/auth-tokens/`, a personal token
  with `org:read`, `project:read` and `event:read`. An organization token's
  fixed scopes are for CI and do not read issues.
- GitLab: `<base_url>/-/user_settings/personal_access_tokens?name=DevX&scopes=read_api`,
  which opens the form with the name and scope filled in.
- Firebase needs no token: *set it up ↗* opens the console's Integrations page,
  where the BigQuery card's *Link* turns the export on.
- BrowserStack: Emulator > BrowserStack has *get your username and access key ↗*.

The link follows the connector's `base_url`, so a self-hosted Sentry or GitLab
opens its own page.

**A credential belongs to one connector**, so it is never sent to another
connector's host. The lookup order:

1. The connector's own Keychain item, `com.devx.<provider>.<connector-id>`
   (for example `com.devx.sentry.sentry-prod`). This is where the window saves
   a credential typed into a connector form; it is never shown again.
2. The provider's shared item, `com.devx.sentry` or `com.devx.gitlab`.
3. `--credential-ref`, if set, replaces both with one item you name.
4. The provider's environment variable (`SENTRY_AUTH_TOKEN`, `GITLAB_TOKEN`),
   but only while the connector is the workspace's one connector of that
   provider. With a gitlab.com and a self-hosted GitLab side by side, one
   variable would otherwise reach both.

Health checks ask whether a credential exists without reading it, so they
never make macOS ask for Keychain access. A sync reads it; macOS asks once,
and *Always Allow* stops the asking.

### What each connector makes of its provider

- **Sentry.** An issue is a `crash` when Sentry's level is fatal or it was
  unhandled, otherwise an `issue`. A release string `com.acme.app@5.4.0+54019`
  gives app, version and build. `dist` also gives the build, and the release's
  `lastCommit` gives the commit. Stack frames are kept crash frame first. The
  first sync reads `lookback_days`; later ones ask only for issues seen since
  the last complete sync, plus a full read of the window once a day so
  resolved and ignored issues are current.
- **GitLab CI.** A pipeline, job or deployment names the commit it ran on, so
  its release identity is **exact**. For a failed job, the log is kept as raw
  evidence and a local, deterministic parser finds the root error. A compiler
  error or failing test outranks Gradle's summary, and the runner's
  `ERROR: Job failed` line is the last resort. The excerpt is the window around
  that line, not the head of a 20 MB log. Merge-request refs are not reported
  as branches. Artifacts are never downloaded.
- **Firebase.** Firebase has no supported API for Crashlytics issues; its
  supported route is the BigQuery export, and DevX reads that. Crash rows are
  aggregated per issue, version and build. The aggregate keeps the distinct
  event count, distinct installations, the blamed frame and the exception.
  Personal fields (`user`, `custom_keys`, `logs`, `breadcrumbs`) never reach
  a record. Performance rows become metrics per trace or URL pattern and build,
  with nearest-rank p50/p90/p95/p99 computed by DevX.
- **JSONL.** Each line is a full `devx.signal/1` record or a looser object.
  A line that claims an `exact` release without a full commit, or without
  version + build + app, is downgraded to `provider_attributed`: certainty
  cannot be imported.

### Syncing

A sync is bounded: pages, records, and bytes per raw file. It ends one of three
ways:

- **complete**: the watermark moves forward, and the next sync asks only for
  what changed;
- **partial**: rate limited, a bound reached, or a page failed. What was
  written is kept, the cursor remembers where to resume, and the watermark
  does not move. The connector shows *partial* until a complete sync, so the
  cache never looks more current than it is;
- **failed**: nothing changed.

`validate` checks the credential and settings; `discover` lists what can be
synced (Sentry projects, GitLab projects). Pausing a connector stops its syncs.
**Disconnecting removes its configuration and keeps its evidence**; deleting
local data is a separate, explicit choice.

## Releases and correlation

A release is whatever the evidence names. Its key is
`<app>@<version>+<build>` when a version is known, otherwise
`commit:<sha>`. Correlation (`core/correlation`) joins:

| basis | when | shown as |
|---|---|---|
| **exact** | the same full commit SHA; or app, version and build all agree | linked |
| **provider_attributed** | a provider states it (a Sentry event names its release); or you linked a session to a release yourself | attributed |
| **candidate** | a short SHA prefix; a version without its build; a production signal with no release that started after a deploy | candidate: investigate |

A signal with an exact link gets no candidate link to another release. When
two sources disagree (two commits for one build) the release shows the
conflict instead of picking one.

**DevX sessions** join a release through the version their capture recorded:
`versionName`/`versionCode` from the device on Android, or the in-app SDK's
`app.version`/`app.build_number`. When a capture has neither,
`mpi intelligence link-session <session> <release>` records your own link,
labelled as yours. BrowserStack App Profiling imports are sessions too.

A release page shows:

- the identity (version, build, commit, branch, first deploy);
- a timeline: pipeline, tests, deploy, first production signal, sessions;
- the evidence by provider;
- the sessions that measured this build;
- exact and candidate links, apart;
- what is missing: no commit, no build, no CI, no deploy, no production
  signal, no session.

**Compare** puts two releases side by side. It shows crash and issue counts,
event and affected-user totals (only where providers reported them; absent is
not zero), CI and test failures, issues only in the newer release, and metric
percentile deltas. A difference in evidence is not a measured cause.

## Code context

With a repository root set, `mpi intelligence code <signal>` (and the
`code_context_for_signal` tool) reads the local git repository:

- the release's commit, or HEAD, said so, when that commit is not in this
  repository;
- the signal's stack frames resolved to tracked files. A frame resolves by the
  longest path suffix that names exactly one file; anything ambiguous is left
  unresolved rather than guessed;
- the lines around each frame, at that commit;
- blame, by author name only (never their e-mail);
- the release's diff against the previous release or the commit's parent,
  limited to those files.

Paths are checked against the repository root before anything is read; `git`
runs with an argv, never a shell, and with no external diff or textconv
drivers. A moved or missing repository makes code context unavailable, not
wrong.

## AI tools

`mpi mcp` (see [MCP server](mcp-server.md)) reads Intelligence from the local
store:

| tool | does |
|---|---|
| `intelligence_projects` | workspaces and how fresh each connector's evidence is |
| `connector_status` | connectors, health, cursor, storage; whether a credential exists, never its value |
| `signals_search` | filter signals by provider, kind, severity, environment, release, commit, time, text |
| `signal_read` | one signal; `raw=true` adds a bounded, redacted excerpt |
| `release_overview` | releases, or one release in full |
| `release_compare` | two releases side by side |
| `signal_related` | one signal's links, with what each rests on |
| `code_context_for_signal` | commit, frames, source, blame, diff |
| `intelligence_evidence_pack` | the starting point: facts, links, excerpts, code context, missing evidence, source refs |
| `connector_sync` | refreshes connectors from their providers; **needs `--allow-network`** |

The three permissions are independent ([ADR-0011](adr/0011-three-tier-mcp-permissions.md)):

```
mpi mcp                     local reads: sessions, observations, signals, releases, code context
mpi mcp --allow-network     + connector_sync
mpi mcp --allow-actions     + recording, booting, operating devices, uploads
```

An **evidence pack** is at most 64 KB:

- `question_scope` and `freshness`;
- `facts`: one line each, citing a local id;
- `exact_links` and `candidate_links`, apart;
- `raw_excerpts`: bounded and redacted;
- `code_context`;
- `missing_evidence` and `source_refs`.

It tells the agent to cite the refs and not to recompute what DevX computed.
In the window, AI Analysis builds the same pack, copies it, and copies a
prompt for your AI tool.

**Redaction.** Before any excerpt or pack leaves the store, DevX replaces
known credential shapes, e-mail addresses and, in excerpts, IP addresses with
a marker naming what was removed. Credentials covered: bearer and basic auth,
JWTs, GitLab, GitHub, Sentry and Slack tokens, AWS and Google keys, private
keys, credentials in URLs, and `password=` / `token=` style assignments.
The scanner knows shapes, not meaning: a secret in a shape it does not know
passes through. That is why raw views carry a warning, and why the raw store
itself never leaves the Mac.

## Retention

Per workspace, overridable per connector: 7, 30 or 90 days, or forever. A
signal expires when DevX last stored it longer ago than that; a provider
still reporting it refreshes it. Raw evidence that no retained record points
at goes with it. **Pinned** signals and releases are kept regardless.
`mpi intelligence retention` shows what would be deleted; `--apply` deletes
it. Storage use, the oldest evidence and quarantined records are shown in
Integrations.

## What leaves this Mac

Settings > *What leaves this machine*, Intelligence > Integrations and
`mpi intelligence egress` list it connector by connector: the Sentry and GitLab
connectors send authenticated API requests to the host you configured, only
when they validate, discover or sync; Firebase and JSONL read local files;
nothing uploads sessions, source or raw evidence anywhere. An AI host gets
what it asks `mpi mcp` for, under the permissions above.

## Status and limits

- **Tested against live services:** GitLab CI, anonymously, on the public
  project `gitlab-org/cli`. That covered validate, sync, partial and resumed
  syncs, exact commit identity, releases, an evidence pack, and MCP reads.
  Sentry and Firebase are tested against fixtures in their documented API and
  export shapes, not against a live account. Firebase's field names were
  checked against Google's published BigQuery schemas.
- **Sentry status changes** are caught by a full read of the lookback window
  once a day. An incremental sync asks only for issues *seen* since the last
  one, and an issue resolved without a new event is not among them, so between
  full reads its local state can be up to a day old.
- **Sentry regions.** Set `base_url` to your region host (for example
  `https://us.sentry.io`) when pagination links point there; links to any other
  host are not followed.
- **Release identity quality** decides everything. Without a commit and build
  in what your CI and crash reporter send, joins are candidates. Report them
  (Sentry `release`/`dist`, GitLab deploys of the release commit).
- **Desktop only.** DevX does not receive webhooks; it pulls when you sync.
- **A sync started from `mpi mcp` runs to its end.** It is bounded: pages, records
  and a timeout per request. But the AI host cannot cancel it halfway; from the
  window or the CLI, cancel works.
- **JSON files, no database.** That is fine for tens of thousands of signals
  per workspace; beyond that is a measured decision for later.
