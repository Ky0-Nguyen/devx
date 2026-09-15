# iOS: what works, what does not, and what was measured

The record of how iOS capture behaves in this tool, and of the evidence behind
each claim. Written because almost every sentence here was wrong at some point
and the corrections are the useful part.

Everything below was measured on this host. Where something is unverified, it
says so.

---

## 1. Current state

| | physical device | simulator |
|---|---|---|
| discovery | works | works |
| app / process enumeration | works (needs a connected device) | works |
| batch capture (`record`) | implemented, **never run against hardware** | xctrace cannot record a simulator target — see §5 |
| live capture | not possible: the app is not a host process | **works** — CPU time, utilisation, memory footprint, per-thread times |
| stack attribution | via xctrace, unverified | **works** — `/usr/bin/sample`, see §7 |
| frame timing | via xctrace, unverified | impossible: no command-line frame source exists |

The simulator path is `adapters/ios/simulator_host_collector.cpp`; it needs no
Instruments at all.

---

## 2. Why the simulator path is possible

An app running in an iOS simulator is an ordinary macOS process owned by the
developer. That is the whole reason, and it does not extend to a device.

Verified on iPhone 17 Pro (26.5), simulator `456FA0D8`:

| What | How | Result |
|---|---|---|
| bundle id → host pid | `simctl spawn <udid> launchctl list`, label `UIKitApplication:<bundleid>[…]`, column 1 | works; the pid is a host pid owned by the user |
| memory | `proc_pid_rusage`, `ri_phys_footprint` | works, no root — and it is the metric Xcode's gauge shows |
| thread count | `proc_pidinfo(PROC_PIDLISTTHREADS)` | works (20 live threads on the RN app) |
| per-thread CPU + **names** | `proc_pidinfo(PROC_PIDTHREADINFO, <handle>)` | works, no root |
| stack attribution | `/usr/bin/sample <pid>` | works — §7 |
| Instruments-grade sampling | `task_for_pid` | refused (kr=5) without root or the debugger entitlement |

`PROC_PIDTHREADID64INFO` does **not** work with the handles
`PROC_PIDLISTTHREADS` returns (errno 3); `PROC_PIDTHREADINFO` is the flavour
that takes them.

### The thread split, from a platform signal

Thread names come back as the runtime's own, not inferred:

```
com.facebook.react.runtime.JavaScript     18981 ms user
hades                                       127 ms   (Hermes GC)
com.apple.uikit.eventfetch-thread            43 ms
com.facebook.SocketRocket.NetworkThread      21 ms
<unnamed, index 0>                         5975 ms   (main)
```

This is the iOS half of the JS-versus-native question, and it qualifies as
`platformSignal` rather than `threadName`: the runtime sets it through
`pthread_setname_np`, we do not pattern-match it.

One thing it exposed: the app carried a `com.apple.rosetta.exceptionserver`
thread, so it was running **translated**. Translated CPU timings are not
comparable to a device, nor to a native simulator build, and a capture that
did not say so would invite exactly that comparison. The collector records it.

---

## 3. The unit trap that blocked this for a while

The task-level CPU counters (`PROC_PIDTASKINFO`, and the identical values in
`proc_pid_rusage`) are in **mach absolute time units**. The per-thread
counters (`PROC_PIDTHREADINFO`) are in **nanoseconds**. Two units in one API
family.

Against a process burning a deliberate 3.0 s:

| Source | Reading |
|---|---|
| task counters read as nanoseconds | 0.072 s |
| task counters read as mach ticks | **2.999 s** |
| `ps -o time` | 3.00 s |

The ratio is the timebase: 125/3 on Apple Silicon, 41.6667 ns per tick — and
1/1 on Intel, where reading them as nanoseconds happens to work, which is how
the trap stays hidden. Confirmed again live: 6.594 s where `ps` independently
said 6.59 s.

This was originally written up as an unexplained fifty-fold discrepancy, with
a guess that the task counters excluded live threads. The guess was wrong;
only the unit was at fault.

---

## 4. `devicectl` answers from a cache — but only one subcommand does

Which matters, because it decides which readings can be trusted as current.
Tested against a device absent for four days:

| command | against an absent device | conclusion |
|---|---|---|
| `device info details` | **`outcome: success`** in ~0.1 s, full properties | answers from a cached CoreDevice record |
| `device info lockState` | fails, CoreDeviceError 1011, ~0.11 s | requires the hardware |
| `device info processes` | fails, 1011, ~0.11 s | requires the hardware |
| `device info apps` | fails, 1011, ~0.08 s | requires the hardware |

So `details` is the only reading that is an observation with a date rather
than a fact about now — it still reported `developerModeStatus: enabled` for a
phone that had been away four days. `connectionProperties.lastConnectionDate`
is what makes that detectable, and it travels with anything read from it as
`DeviceRef::last_seen_at`.

The three that need hardware are why a device authorized at discovery can
still fail at enumeration: the phone was unplugged in between.
`describe_devicectl_failure` names that rather than printing Apple's raw
error, and keeps 1011 (device gone) distinct from 1000 (not a CoreDevice
device at all — a simulator UDID, or a typo).

`lockState` is also the live reachability probe: it has to reach the hardware,
fails in ~114 ms when there is nothing to reach, and reports lock state, which
a capture needs anyway since a locked device cannot be driven.

---

## 5. `xctrace` cannot record an iOS simulator target on this host

Isolated by comparison, because the same command behaves completely
differently depending on what it is pointed at:

| target | result |
|---|---|
| plain macOS process | honours `--time-limit`, prints `Ctrl-C to stop the recording`, exits by itself, writes a bundle that exports |
| simulator, `--attach <pid>` | accepts the target, never prints `Ctrl-C to stop`, never reaches its own time limit, ignores SIGINT, leaves a 52 KB stub that exports with `Document Missing Template Error` |
| simulator, `--launch <executable>` | identical hang |
| simulator, `--launch <bundle id>` | launch *fails*, and then the recording completes and exits normally |

That last row is the tell: xctrace finishes cleanly whenever there is nothing
to record and hangs as soon as it has a live simulator target. The fault is in
recording a simulator target, not in the invocation.

Two things changed as a result.

**`proc::Options` gained `stop_signal` and `stop_grace`.** Instruments
finalises its bundle on SIGINT — it says so — and needs seconds. The default
SIGTERM-then-SIGKILL-in-500ms destroyed recordings that had completed but not
exited. That is a real data-loss path on a physical device even though it does
not rescue the simulator case. Tested without hardware:
`tests/helpers/fake_finalising_tool.cpp` ignores SIGTERM and writes its output
only on SIGINT, after a delay, which is what Instruments does.

**A timeout no longer refuses on its own.** "Is there a usable trace?" is
answered by trying to read one — `ios::bundle_is_readable` runs
`xctrace export --toc`, which rejects a stub in under a second — rather than
by how the process ended. Checked against the three bundles these experiments
produced:

| bundle | what it is | verdict |
|---|---|---|
| 10 MB, macOS recording | a real capture | readable |
| 52 KB, simulator attach | a stub | rejected: *Document Missing Template Error* |
| 252 KB, recording ran but the launch failed | no run data | rejected: *Trace is malformed - run data is missing* |

The third is why the check runs `--toc` rather than looking at size or
existence: that bundle is five times the stub and contains nothing.

---

## 6. Two reasons a capture can look empty on a good device

**Full Disk Access.** Instruments samples through the unified log store, and a
process without Full Disk Access cannot open it:

```
$ /usr/bin/log show --last 5s
log: Could not open local log store: Operation not permitted
$ ls -ld /var/db/diagnostics
drwxr-x---  17 root  admin
```

xctrace reports that as `Fatal logging system error: The log archive is
corrupt or incomplete and cannot be read`, which describes a damaged machine
and is usually this permission. It is reported as `permission_denied` with the
fix, in the capture and in `mpi preflight` as `ios.capture.log_store`, so it
is found before a capture is attempted rather than after.

**`time-profile` is not the only table with samples.** The collector exports
`table[@schema="time-profile"]`. On a real recording here:

| table | rows |
|---|---|
| `time-profile` | **0** |
| `time-sample` | **2** — a real thread, process and kperf backtrace |
| `kdebug` | 40 |
| `os-log` | 0 |

The samples were in the bundle, in a schema this build does not parse. An
empty `time-profile` now triggers a row count over the tables that plausibly
carry samples, and reports that the samples exist, how many, in which schema,
and where the bundle is — because a gap in this tool is not an empty capture.

---

## 7. Stack attribution on a simulator

This document previously concluded, from `task_for_pid` being refused, that
stack attribution was impossible here. That was wrong, and the wrong claim was
in the collector and its capability output as well.

`/usr/bin/sample` is entitled to do what this process cannot:

```
$ /usr/bin/sample 74447 2
Call graph:
    1702 Thread_6788599   DispatchQueue_1: com.apple.main-thread  (serial)
    + 1702 start  (in dyld) + 6992
    +   1702 start_sim  (in dyld_sim) + 20
    +     1702 ???  (in MobileCal) load address 0x102350000 + 0x1348a0
    +       1702 UIApplicationMain  (in UIKitCore) + 120
    +         1702 -[UIApplication _run]  (in UIKitCore) + 776
    +           1702 GSEventRunModal  (in GraphicsServices) + 116
```

1294 lines of symbolised call graph with per-thread attribution, in under four
seconds, no root and no Full Disk Access.

`adapters/ios/sample_parser.cpp` turns that into weighted `CpuSample`s, behind
`CaptureConfig::stack_profile` and a checkbox in the Live tab. Two things in
the parsing are easy to get wrong and invisible when they are:

**Each node's count includes its children,** so the quantity that means
anything is `self = count - sum(children)`. Emitting every node at its full
count multiply-counts the same samples down the path; emitting only leaves
loses every frame with both self time and callees. The test for this is
constructed, and labelled so, because the recorded graph is single-child
chains throughout and cannot exercise it.

**A frame must appear in its own stack.** Emitting a node after popping it off
the path left every sample one frame short — the deepest stack ended at the
*caller* of its leaf. That conserves the weights, so a sample-count check
passes and the stacks are quietly wrong. Caught by checking the leaf against
the recording: `mach_msg2_trap`, not `mach_msg2_internal`.

The conservation check is worth keeping as the other half: self times must sum
exactly to what the threads declared. Verified at 37444 samples across 22
threads, and on `fixtures/sample/recorded-simulator-callgraph.txt`.

What it cannot do is place any of this in time. `sample` reports an aggregate
with no timestamps, so it answers "where" and never "when", and the capability
says so.

---

## 8. What is still unverified, and why

**No capture has ever run against a connected physical iPhone.** The two
devices here were last seen 2025-09-11 and 2025-06-25. Every physical-device
claim in this tool rests on code review and on the probes above correctly
reporting them absent — not on a successful capture. The SIGINT fix in §5 is
aimed squarely at that path and has only ever been exercised against a
stand-in.

The physical-device *enumeration* path has been executed, with a recorded
`devicectl` — see `tests/helpers/devicectl-stub`. That harness found three
bugs in code that had never run, including process attribution never working
at all.

**Ingesting a populated `time-profile` export is unexercised**, because of the
Full Disk Access problem in §6. Every recording here has a schema and no rows.

Both of those are properties of this machine, not of the code, and the notes
that used to describe them as machine faults were wrong twice — once about a
"corrupt" log archive, once about sampling being impossible.
