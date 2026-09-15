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

## What it cannot tell you

The device-side executable path. `processes.json` carries the real *simulator*
container shape, which is verifiable here; a physical device's shape is not,
which is exactly why an unmatched attribution now reports `unknown` rather
than `not_running`.

And it cannot test a capture: `xctrace` needs real hardware, and no stub can
stand in for that.
