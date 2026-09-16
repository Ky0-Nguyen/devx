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

Started as above, the server **refuses to change anything**. Twelve tools
read; three do not, and they are visible in the catalogue with their
descriptions saying they are unavailable and naming the flag that enables
them.

```json
{ "args": ["mcp", "--allow-actions"] }
```

That enables `record_capture` (starts processes on a device and writes a
session package), `boot_device` (changes this machine's state) and
`observe_app` (attaches a debugger to a running app, and with `detail` on
captures headers including bearer tokens).

The split is not decoration. A model that decides on its own to "just try
recording" is a different thing from a person asking for it, and a recording
cannot be un-started. Refused calls say plainly that nothing was done.

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
| `record_capture` | **acts** — records and analyses a new capture |
| `boot_device` | **acts** — starts a simulator or emulator |
| `observe_app` | **acts** — attaches a debugger for a window |

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
