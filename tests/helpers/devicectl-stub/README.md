# A recorded `devicectl` for the physical-device path

**Every file here is SYNTHETIC.** The device it describes is named
`SYNTHETIC connected iPhone` so that nothing downstream — a session package, a
report, a screenshot — can be mistaken for a capture from real hardware.

## Why it exists

The physical iOS device path had never executed. Both devices available here
were last seen 2025-09-11 and 2025-06-25, so every code path that runs only
for a *connected, prepared* device — the readiness gate, app enumeration,
process attribution, runtime state — had never once run.

This is not a simulation of an iPhone. It replays **recorded response shapes**
for the four `devicectl` subcommands the adapter uses, with the connection
fields set to what a connected and prepared device reports:

    tunnelState           connected
    pairingState          paired
    ddiServicesAvailable  true
    developerModeStatus   enabled

The shapes were taken from real `devicectl --json-output` runs against the
actual hardware; only those four fields were changed.

## Using it

`xcrun` is resolved through PATH by `posix_spawnp`, so putting this directory
first is enough — and anything that is not `devicectl` is forwarded to the
real `/usr/bin/xcrun`:

```bash
PATH=tests/helpers/devicectl-stub:$PATH ./build/bin/mpi devices --no-simulators
PATH=tests/helpers/devicectl-stub:$PATH ./build/bin/mpi apps \
    --device 3FF46431-775C-59BB-AD26-D316DFAFA5A6 --no-simulators
```

## What it found on its first run

Process attribution never worked. The rule required the bundle id as a whole
path segment (`"/" + bundle_id + "/"`), and no real container path has that:
the real shape is `…/Application/<UUID>/io.pizzahut.hutbot.debug-1789449967523.app/HutBot`,
where the bundle id is a segment *prefix*, and a physical device's path is
`…/<UUID>/<Product>.app/<Product>`, which does not contain the bundle id at
all. So every app on a physical device reported `not_running` with no
processes — and a capture would have told someone to start an app that was
already running.

## It also stubs `xctrace record`

`record` copies a **donor trace bundle** and forwards `export` to the real
`xctrace`, so the whole capture path runs: record, the readability check, the
export, the ingest, and the decision about whether a session is written. The
donor is not committed -- it is 10 MB of binary, and this tree has none.

Make one from any macOS process:

```bash
xcrun xctrace record --no-prompt --template "Time Profiler" \
  --attach <pid> --output tests/helpers/devicectl-stub/donor.trace \
  --time-limit 5000ms
```

The samples in it are real and are **not from an iOS device**. That is why
this is a harness and not a fixture: it exercises code, and nothing it
produces may be presented as a capture from hardware.

### What running it established

The capture path works end to end and degrades honestly. With a donor whose
`time-profile` table has a schema and no rows, the capture reported

    ios.capture.live_recording: limited -- the exported table held no samples
      limitation: the recording and the export both succeeded, so this is an
      empty or unreadable table rather than a missing provider

and wrote **no session**, which is right: a session with no samples would be a
capture-shaped artifact with nothing in it.

It also confirmed that parsing `Output file saved as:` is necessary rather
than defensive. xctrace given `--output /abs/path/donor2.trace` answered
`Output file saved as: donor2.trace` -- a relative path, landing in the
working directory rather than where it was asked to go.

### Why the populated-ingest path is still unexercised here

Every recording taken here comes back with a `time-profile` table that has a
schema and zero rows, for a plain macOS process as much as for anything else,
and xctrace says:

    [Error] Data stream: Fatal logging system error: The log archive is corrupt
            or incomplete and cannot be read

**That wording is misleading and I believed it at first.** The archive is not
corrupt. Instruments samples through the unified log store, and this process
cannot open it:

    $ /usr/bin/log show --last 5s
    log: Could not open local log store: Operation not permitted

    $ ls -ld /var/db/diagnostics
    drwxr-x---  17 root  admin  /var/db/diagnostics

It is a **permission**: no Full Disk Access. Granting it to whatever runs the
tool -- or running the capture from Xcode, which already has it -- is very
likely to make sampling work, which means "iOS capture cannot be verified
here" was a statement about this execution context and not about the machine.

The collector now says this itself: an empty table triggers one
`log show --last 1s`, and when that is refused the capability is reported as
`permission_denied` with the fix, rather than as an empty table.

## What it cannot tell you

The device-side executable path. `processes.json` carries the real *simulator*
container shape, which is verifiable here; a physical device's shape is not,
which is exactly why an unmatched attribution now reports `unknown` rather
than `not_running`.

And it cannot test a capture: `xctrace` needs real hardware, and no stub can
stand in for that.
