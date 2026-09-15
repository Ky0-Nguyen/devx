# iOS live capture: what is actually reachable

Recorded mid-investigation so the measurements are not lost. This is
reconnaissance, **not** an implemented provider: nothing in this document is
wired into the app yet, and one question in it is unresolved.

## The opening that makes this possible

An app running in an iOS **simulator** is an ordinary macOS process owned by
the developer. That is the whole reason a live provider is conceivable here at
all, and it does not extend to a physical device.

Verified on iPhone 17 Pro (26.5), simulator `456FA0D8`:

| What | How | Result |
|---|---|---|
| bundle id → host pid | `simctl spawn <udid> launchctl list`, label `UIKitApplication:<bundleid>[…]`, column 1 | works; the pid is a host pid owned by the user |
| memory | `proc_pid_rusage`, `ri_phys_footprint` | works, no root — and `phys_footprint` is the metric Xcode's gauge shows |
| thread count | `proc_pidinfo(PROC_PIDLISTTHREADS)` | works (20 live threads on the RN app) |
| per-thread CPU + **names** | `proc_pidinfo(PROC_PIDTHREADINFO, <handle>)` | works, no root |
| Instruments-grade sampling | `task_for_pid` | **refused** (kr=5) without root or the debugger entitlement |

`PROC_PIDTHREADID64INFO` does *not* work with the handles that
`PROC_PIDLISTTHREADS` returns (errno 3); `PROC_PIDTHREADINFO` is the flavour
that takes them.

## The thread split, from a platform signal

Thread names on the RN app came back as the runtime's own, not inferred:

```
com.facebook.react.runtime.JavaScript     18981 ms user
hades                                       127 ms   (Hermes GC)
com.apple.uikit.eventfetch-thread            43 ms
com.facebook.SocketRocket.NetworkThread      21 ms
<unnamed, index 0>                         5975 ms   (main)
```

This is the iOS half of the JS-versus-native question, and it qualifies as
`RoleBasis.platformSignal` rather than `threadName`: the name is set by the
runtime through `pthread_setname_np`, not pattern-matched by us.

One thing this exposes and must be surfaced if it is ever used: the app carried
a `com.apple.rosetta.exceptionserver` thread, meaning it was running
**translated** under Rosetta. Translated CPU timings are not comparable to a
device, nor to a native simulator build, and a capture that did not say so
would be inviting exactly the comparison the project forbids.

## RESOLVED: the CPU numbers were in the wrong unit

This section recorded an unexplained fifty-fold discrepancy and refused to
build a CPU provider on it. The explanation is a unit.

The task-level counters (`PROC_PIDTASKINFO`, and the identical values in
`proc_pid_rusage`) are in **mach absolute time units**. The per-thread
counters (`PROC_PIDTHREADINFO`) are in **nanoseconds**. Two units in one API
family.

The ratio is the host timebase: 125/3 on Apple Silicon, or 41.6667 ns per
tick, which is the factor of ~42 that looked like 50. On Intel it is 1/1,
which is why reading the counters as nanoseconds works there and hides the
trap entirely.

With the timebase applied, against a process burning a deliberate 3.0 s:

| Source | Reading |
|---|---|
| task counters read as nanoseconds | 0.072 s |
| task counters read as mach ticks | **2.999 s** |
| `ps -o time` | 3.00 s |

Confirmed again live against the simulator's Calendar app: 6.594 s where `ps`
independently reported 6.59 s.

`adapters/ios/simulator_host_collector.cpp` is the provider this unblocked.
The earlier guess in this document -- that the task counters exclude live
threads -- was wrong; they include everything, and only the unit was at fault.

## Also worth knowing: `devicectl` answers from a cache

`devicectl device info details --device <id>` returns `outcome: success`, in
about a tenth of a second, for a device that is **not present at all**. Every
property it reports is then a description of the device as it was when last
seen. Measured on a phone that had been away four days: it still reported
`developerModeStatus: enabled`.

`connectionProperties.lastConnectionDate` is what makes this detectable, and
it is now carried as `DeviceRef::last_seen_at` so those properties are never
presented as current facts.

For a live answer, `devicectl device info lockState` has to reach the
hardware: it fails in ~100 ms with CoreDeviceError 1011 when there is nothing
to reach, and it also reports whether the device is locked -- which a capture
needs anyway, since a locked device cannot be driven. That is
`ios::probe_reachability`.

## Historical: the reasoning that held this up



Three readings of the same process, taken at one instant, disagree — and a CPU
series built on the wrong one would be a fabricated measurement.

Against a purpose-built process that burned a known ~3.0 s of CPU across one
exited thread, one live thread and the main thread:

| Source | Reading |
|---|---|
| `proc_pid_rusage` (`ri_user_time + ri_system_time`) | 0.059 s |
| `proc_pidinfo(PROC_PIDTASKINFO)` (`pti_total_*`) | 0.059 s — identical |
| sum over live threads | 0.797 s |
| `ps -o time` | 2.47 s |

So `proc_pid_rusage` and the task-level totals agree with each other and
disagree with reality by roughly fifty-fold. The task-level counters appear to
exclude live threads, which is long-standing macOS behaviour, but
`task-level + live threads` still only reaches 0.86 s against a true 2.5–3.0 s,
so that explanation is incomplete.

**Until this is understood, there is no CPU provider here.** Memory
(`phys_footprint`) is unaffected and stood up to checking; the thread names are
not timing at all. A provider could honestly ship memory and the thread split
with CPU declared a missing provider, which is what the coverage model exists
to express.

## Physical iOS devices

Out of reach by this route entirely: the app is not a host process, and
`xctrace record` produces a bundle only when it finishes — on this host it
attaches and then does not finish. `XctraceCollector::supports_streaming()`
reporting `false` remains correct for physical devices.

## `xctrace` cannot record an iOS simulator target on this host

Isolated by comparison rather than assumed, because the same command behaves
completely differently depending on what it is pointed at:

| target | result |
|---|---|
| plain macOS process | honours `--time-limit`, prints `Ctrl-C to stop the recording`, exits by itself, writes a 10 MB bundle that `xctrace export --toc` reads |
| iOS simulator, `--attach <pid>` | accepts the target, never prints `Ctrl-C to stop`, never reaches its own time limit, ignores SIGINT, leaves a 52 KB stub that exports with `Document Missing Template Error` |
| iOS simulator, `--launch <executable>` | identical hang |
| iOS simulator, `--launch <bundle id>` | launch *fails* (`posix_spawn failure … No such file or directory`), and then the recording completes and exits normally |

That last row is the tell: xctrace finishes cleanly whenever there is nothing
to record, and hangs as soon as it actually has a live simulator target. The
fault is in recording a simulator target, not in the invocation.

Two things changed as a result.

`proc::Options` gained `stop_signal` and `stop_grace`. Instruments finalises
its bundle on **SIGINT** -- it says so -- and needs seconds to write it, so
the default SIGTERM-then-SIGKILL-in-500ms was destroying any recording that
had completed but not exited. That is a real data-loss path on a physical
device even though it does not rescue the simulator case.

That fix is tested without hardware, because the behaviour that caused the
loss can be reproduced exactly. `tests/helpers/fake_finalising_tool.cpp`
ignores SIGTERM and writes its output only on SIGINT, after a delay, which is
what Instruments does. Four cases pin it:

- with the **default** stop signal the output is lost -- a demonstration that
  the bug was real rather than theoretical;
- with SIGINT and a grace period the output survives;
- a grace period shorter than the child's work still ends the child, because
  a collector that hangs on shutdown is worse than a lost bundle;
- a child that exits promptly is not delayed by a long grace period, so the
  twelve-second budget costs nothing when it is not needed.

A fifth case asserts that the xctrace collector asks for those settings.
`proc::run` honouring a stop signal and this collector choosing the right one
are two different things, and only the second prevents the loss -- so the
policy is exposed as `ios::xctrace_stop_signal()` and
`ios::xctrace_stop_grace()` and pinned directly.

And a timeout no longer refuses on its own. The question "is there a usable
trace?" is now answered by trying to read one -- `xctrace export --toc`
rejects a stub in well under a second -- rather than by how the process ended.
A capture xctrace finished but did not exit from is kept and marked partial.

That decision is `ios::bundle_is_readable`, extracted from the capture path
because inline it was reachable only with a device attached. It was checked
against the three bundles these experiments actually produced:

| bundle | what it is | verdict |
|---|---|---|
| 10 MB, from a macOS recording | a real capture | **readable** |
| 52 KB, from a simulator attach | a stub | rejected: *Document Missing Template Error* |
| 252 KB, recording ran but the launch failed | no run data | rejected: *Trace is malformed - run data is missing* |

The third row is why the check runs `--toc` rather than looking at whether a
file exists or how large it is: that bundle is substantial and contains
nothing, and a naive check would have salvaged it as a capture.

For a simulator, the working path is `SimulatorHostCollector`: it reads the
app as a host process and needs no Instruments at all.

## Which `devicectl` commands answer from cache

Worth knowing precisely, because it decides which readings can be trusted as
current. Tested against a device that had been absent for four days:

| command | against an absent device | conclusion |
|---|---|---|
| `device info details` | **`outcome: success`** in ~0.1 s, full properties | answers from a cached CoreDevice record |
| `device info lockState` | fails, CoreDeviceError 1011, ~0.11 s | requires the hardware |
| `device info processes` | fails, CoreDeviceError 1011, ~0.11 s | requires the hardware |
| `device info apps` | fails, CoreDeviceError 1011, ~0.08 s | requires the hardware |

So `details` is the only one whose values are an observation with a date
rather than a fact about now, which is why `lastConnectionDate` travels with
anything read from it. And the three that need the hardware are why a device
authorized at discovery can still fail at enumeration: the phone was unplugged
in between. `describe_devicectl_failure` names that case rather than printing
Apple's raw error, and distinguishes it from error 1000, which means the
identifier is not a CoreDevice device at all.

## Instruments needs Full Disk Access, and says "corrupt" when it lacks it

Worth knowing before concluding anything about a machine's ability to
profile. Every recording taken in this environment produced a `time-profile`
table with a schema and zero rows, and xctrace explained it as:

    Fatal logging system error: The log archive is corrupt or incomplete and
    cannot be read

Nothing is corrupt. Instruments samples through the unified log store, and a
process without Full Disk Access cannot open it:

    $ /usr/bin/log show --last 5s
    log: Could not open local log store: Operation not permitted
    $ ls -ld /var/db/diagnostics
    drwxr-x---  17 root  admin

So an empty capture on a perfectly good device can be a checkbox rather than a
fault, and the wording actively misdirects. Two places now say so: a capture
that comes back with an empty table probes `log show --last 1s` and reports
`permission_denied` with the fix, and `mpi preflight` carries
`ios.capture.log_store` so the permission is found before a capture is
attempted rather than after.

This also means every "unverified on a physical device" note in this document
is a statement about *this execution context* as much as about the absence of
hardware. With Full Disk Access granted, sampling may simply work.

## `time-profile` is not the only table with samples in it

The collector exports `table[@schema="time-profile"]` and treats an empty
result as "no samples". Measured on a real recording from this host:

| table | rows |
|---|---|
| `time-profile` | **0** |
| `time-sample` | **2** -- a real thread, process and kperf backtrace |
| `kdebug` | 40 |
| `os-log` | 0 |
| `dyld-library-load` | 0 |

The samples were in the bundle the whole time, in a schema this build does not
parse. "The exported table held no samples" was true and misleading at once,
and it is the same shape as refusing a recording xctrace had finished but not
exited from: data present, discarded on the strength of the wrong question.

Parsing `time-sample` is not done here, and deliberately. Its columns are
different -- `cp-user-callstack`, `kperf-bt`, `thread-state` rather than
time-profile's weighted frames -- and the only sample available to write it
against is two rows from a recording that failed. A parser written against
that and unverifiable against a good capture is worse than none.

What the collector does instead is say so: an empty `time-profile` triggers a
row count on the tables that plausibly carry samples, and when one has rows it
reports that the samples exist, how many, in which schema, that this build
does not read it, and where the bundle is so it can be opened in Instruments.
A gap in this tool is not an empty capture, and the two must not read alike.

The `os-log` count of zero in that table is the Full Disk Access problem above,
visible from a second angle.
