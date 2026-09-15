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

## Unresolved: which number is total CPU

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
