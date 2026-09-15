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
