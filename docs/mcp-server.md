# Reading a capture from an AI tool

`mpi mcp` is a [Model Context Protocol](https://modelcontextprotocol.io)
server. An editor or agent launches it as a subprocess, and the model on the
other side can list captures, read a whole report, ask what a detector's
thresholds are, and — if you let it — record a new capture.

The point is to stop pasting screenshots. A model looking at a picture of a
finding has the title and nothing else; a model holding the report has the
evidence refs, the threshold that fired, the coverage gaps and the provenance
of every source.

The server calls itself **DevX** when a host asks, and `devx` is the key to register it under. The binary is still `mpi`: one CLI, and `mcp` is one of its commands.

## Setting it up

The server speaks JSON-RPC 2.0 over stdio. There is no port and nothing
listening.

**Claude Code** — one command, no file to edit:

```bash
claude mcp add -s user devx /absolute/path/to/mpi mcp
```

`-s user` registers it for every folder. Without it the server is scoped to
the directory you happened to run the command in, which is easy to miss --
`claude mcp list` shows which you got.

**Claude Desktop** — `~/Library/Application Support/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "devx": {
      "command": "/absolute/path/to/mpi",
      "args": ["mcp"]
    }
  }
}
```

**Cursor** — `.cursor/mcp.json` in the project, or `~/.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "devx": {
      "command": "/absolute/path/to/mpi",
      "args": ["mcp"]
    }
  }
}
```

**Codex CLI** — `~/.codex/config.toml`:

```toml
[mcp_servers.devx]
command = "/absolute/path/to/mpi"
args = ["mcp"]
```

The path to prefer is the copy inside the app bundle -- `DevX.app/Contents/MacOS/mpi` -- because it does not move when the build tree does. DevX's own Help tab prints it and offers each of these snippets with a copy button, already filled in.

An absolute path, in every case. A host launches this without a shell, so
`mpi` on your `PATH` is not `mpi` on its `PATH` — the same trap that made the
Devices tab report zero Android devices while `adb` worked fine in a terminal
(see `known-limitations.md`). `--sessions-dir` is worth adding to `args` if
your captures are not in the default location.

## Reading is free; acting is not

Started as above, the server **refuses to change anything** and reaches no
external provider. Thirty-two of its 47 tools read; fourteen act and one
refreshes evidence from a provider. All fifteen stay visible in the catalogue,
and their descriptions say they are unavailable and name the flag that
enables them.

```json
{ "args": ["mcp", "--allow-actions"] }
```

That enables `record_capture` (starts processes on a device and writes a
session package), `boot_device` (changes this machine's state),
`observe_app` (attaches a debugger to a running app, and with `detail` on
captures headers including bearer tokens), `relaunch_with_layout_probe`
(restarts an app on an iOS simulator with the layout probe injected, losing
its state), `device_tap`, `device_swipe`, `device_type_text` and `device_key`
(operate an Android device or emulator), `emulator_start` and `emulator_stop`,
`browserstack_upload` (sends a file to a third party), and
`browserstack_session_start`, `browserstack_session_input` and
`browserstack_session_stop` (run and operate a real BrowserStack device,
spending BrowserStack minutes).

The split is not decoration. A model that decides on its own to "just try
recording" is a different thing from a person asking for it, and a recording
cannot be un-started. Refused calls say plainly that nothing was done.

### Refreshing Intelligence evidence: `--allow-network`

```json
{ "args": ["mcp", "--allow-network"] }
```

The Intelligence tools read production and CI/CD evidence from the local
store, so they answer offline with no flag. `connector_sync` refreshes that
store from Sentry or GitLab with credentials DevX holds, which the model never
sees. It reaches an external service but operates nothing, so it has its own
flag, independent of `--allow-actions` ([ADR-0011](adr/0011-three-tier-mcp-permissions.md)).
See [Intelligence](intelligence.md#ai-tools).

## What the tools return

The C ABI's own JSON documents, unchanged. That is deliberate: re-summarising
a report for a model would mean deciding on its behalf which fields matter,
and the fields a summary drops first are the ones this tool exists for — the
coverage notes, the `not_measured` markers, the difference between a detector
that ran and found nothing and one that could not run.

So a model reading `read_session` sees the same document the desktop app
renders, including every absent value that is absent rather than zero.

| tool | what it answers |
|---|---|
| `mpi_version` | which build produced a report |
| `list_sessions` | every capture on this machine — start here |
| `read_session` | one capture in full: trace, findings, evidence, coverage |
| `session_timeline` | binned tracks, where a blank bin means NOT MEASURED |
| `describe_detectors` | every detector, its thresholds, and where each came from |
| `list_devices` | devices and simulators, with the trust state of each |
| `list_apps` | apps on one device |
| `preflight` | what can and cannot be captured, probed rather than assumed |
| `compare_runsets` | two run sets, gate status before verdict |
| `inspect_targets` | which running apps can be observed through Metro |
| `analyze_trace` | a trace file directly, no session package |
| `list_boot_targets` | simulators and emulators that could be started |
| `list_observations` | saved layout snapshots and inspect observations, newest first, each with a summary |
| `read_observation` | one saved observation in full: every view of a layout, every request of an inspect |
| `capture_layout` | how the screen an app is showing is built, right now; saved, restarts nothing |
| `device_screenshot` | a device's or emulator's screen **as an image the model sees**, with its size |
| `android_avds` | Android virtual devices, their screen sizes, and which are running |
| `devx_window_state` | what the open DevX window is showing |
| `devx_window_show` | show a tab, session, issue, layout snapshot, device or emulator in the DevX window; an emulator is added beside any already shown |
| `browserstack_devices` | real devices on your BrowserStack account |
| `browserstack_sessions` | App Automate builds and sessions; kept as an observation |
| `browserstack_import_profiling` | BrowserStack's App Profiling of one session, written as a DevX session |
| `browserstack_session_screenshot` | the screen of a running BrowserStack session, as an image |
| `intelligence_projects` | Intelligence workspaces and how fresh each connector's evidence is |
| `connector_status` | a workspace's connectors: health, cursor, storage; whether a credential exists, never its value |
| `signals_search` | production and CI/CD signals, filtered by provider, kind, severity, environment, release, commit, time, text |
| `signal_read` | one signal; `raw=true` adds a bounded, redacted excerpt of the provider's evidence |
| `release_overview` | releases, or one release: identity, timeline, evidence, sessions, exact and candidate links, what is missing |
| `release_compare` | two releases side by side: crashes, CI failures, metric deltas |
| `signal_related` | one signal's links, each with the evidence it rests on |
| `code_context_for_signal` | the release's commit, stack frames resolved to files, source, blame, diff |
| `intelligence_evidence_pack` | a bounded, cited, redacted evidence pack for a release or a signal — start here |
| `connector_sync` | **network** — refreshes connectors from their providers; needs `--allow-network` |
| `record_capture` | **acts** — records and analyses a new capture |
| `boot_device` | **acts** — starts a simulator or emulator |
| `observe_app` | **acts** — attaches a debugger for a window; the observation is saved |
| `relaunch_with_layout_probe` | **acts** — restarts a simulator app with the layout probe, then snapshots it |
| `device_tap` · `device_swipe` · `device_type_text` · `device_key` | **act** — operate an Android device or emulator through adb |
| `emulator_start` · `emulator_stop` | **act** — boot or shut down an Android virtual device |
| `browserstack_upload` | **acts** — uploads an app to BrowserStack |
| `browserstack_session_start` · `browserstack_session_input` · `browserstack_session_stop` | **act** — start, operate (tap, swipe, type, key) and stop a real BrowserStack device; stop can import its App Profiling |

## Operating a device

With `--allow-actions`, a model can drive an app the way a tester does:
`device_screenshot` to see the screen, `capture_layout` to know where every
element is (each view's frame, in the same pixels), then `device_tap`,
`device_swipe`, `device_type_text` and `device_key`. It works on any Android
device or emulator through adb. An iOS simulator gets screenshots and layout
but no input, because there is no command-line route for touches without
installing a helper on it, and the tools say so rather than pretend.

Unlike tools that only drive a device, the same server can capture
performance while it does (`record_capture`), read the app's network,
console and Redux (`observe_app`), and keep all of it for later
(`list_observations`).

## Showing things in the DevX window

`devx_window_state` and `devx_window_show` reach the DevX window you have
open, so a model can put the evidence in front of you: the session and issue
it is talking about, the timeline, a layout snapshot, the emulator.

- DevX listens on **127.0.0.1 only**, on a port chosen at start, behind a
  token made at start. Both are written to
  `~/Library/Application Support/DevX/control.json`, mode `0600`, which is
  how `mpi mcp` finds the window.
- A command only changes **what the window shows**. Nothing is recorded,
  started or changed on a device, which is why these are read tools.
- Every command puts a notice across the top of the window, **"An AI tool
  showed you …"** with the model's `reason`, so you always know the window
  moved because a model moved it.
- With DevX closed, the tools say so; they do not open it.

## What is kept for later

A capture is a session package and has always been on disk. The other
results were not: a layout snapshot or an inspect observation existed on
screen or on stdout and was gone afterwards, so a model could only look at
what a person pasted.

Now each one is kept as an *observation* under
`<sessions dir>/observations/<id>.json`, whoever took it: `mpi layout`,
`mpi inspect`, DevX's Layout and Inspect tabs, and the `capture_layout`,
`relaunch_with_layout_probe` and `observe_app` tools. `list_observations`
finds them and `read_observation` returns one whole, so a later conversation
can work from a snapshot taken yesterday without attaching to anything.

- An observation is an envelope (`schema: devx.observation/1`, `id`, `kind`,
  `saved_at`, `app_identifier`, `device_id`, a small `summary`) around the
  same document the command prints with `--json`. A layout observation keeps
  every view, which the command only prints with `--tree`.
- The directory is `0700` and every file `0600`. An inspect observation taken
  with `detail` holds request headers, bearer tokens included, verbatim, and
  its summary says `contains_headers_or_bodies: true`.
- `--no-save` on `mpi layout` and `mpi inspect` skips it. Nothing is ever
  uploaded; these are local files.
- A read tool that keeps what it read is still a read tool: the observation
  is the tool's own record, not a change to the device or the app.

Compare and preflight results are not kept: both are recomputed from their
inputs (two run-set files, a device probe), so `compare_runsets` and
`preflight` give a model the same answer on demand.

## What it does not implement

Listed rather than discovered:

- **stdio only.** No SSE, no HTTP. A local tool talking to a local editor
  needs neither, and each would be an attack surface on a process that can
  read every capture on the machine.
- **Tools only.** No resources, prompts, sampling, roots or completion.
  Nothing here would fill them, and advertising a capability that returns an
  empty list is worse than not advertising it.
- **No batching.** The spec allows a JSON-RPC batch; no host sends one to a
  stdio server, so a batch is refused with a message saying so rather than
  half-handled.
- **No notifications out.** The tool catalogue is fixed at start-up, so
  `listChanged` is advertised as false rather than promising a message that
  never comes.

## Why it is written here

ADR-0002 keeps third-party libraries out of this tree, and the official MCP
SDKs are TypeScript and Python — neither of which this project has a runtime
for. What MCP needs over stdio is JSON-RPC 2.0 with newline-delimited
messages, and this repo already has a JSON parser and writer, so
`core/mcp/protocol.{hpp,cpp}` is about two hundred lines and
`apps/cli/command_mcp.cpp` is the transport plus the tool catalogue.

The dispatcher takes a parsed message and returns the response to write, with
no I/O of its own, which is why `tests/unit/test_mcp.cpp` drives the whole
protocol by handing it JSON. The three things hosts are unforgiving about each
have a test: a reply to a notification can drop the connection, a server that
answers before `initialize` trains hosts to skip the handshake, and a mutating
tool that runs when it should not cannot be undone.
