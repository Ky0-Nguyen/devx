#!/usr/bin/env python3
"""Generates the 1 GiB stress fixture required by specification section 15.

The fixture is not committed (it is a gigabyte). Generate it on demand:

    python3 tools/gen-stress-fixture.py /tmp/stress-1gib.mpi.json
    ./build/bin/mpi analyze /tmp/stress-1gib.mpi.json --format json --out /dev/null

Measured on macOS 26.6.2 / Apple M4 Pro / 48 GB: 12.5 s, 10.2 GB peak RSS for
2,920,000 events. The memory cost is a known defect; see
docs/known-limitations.md section 3.
"""
import random
import sys

PROC = "android|serialX|com.example.perf|pid=4242|start=918273"
THREAD = PROC + "|tid=4242"
TARGET_BYTES = 1024 ** 3


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    rng = random.Random(3)
    leaves = [f"com.example.perf.mod{n}.Frame{n}.render" for n in range(40)]

    with open(out, "w") as f:
        f.write('{"schema_version":"2.0","session_id":"stress-1gib",'
                '"synthetic":true,'
                '"synthetic_note":"1 GiB stress fixture per specification '
                'section 15. Every value is invented.",'
                '"primary_clock_domain":"android.boottime",'
                '"clock_domains":[{"id":"android.boottime","base":"monotonic",'
                '"provider":"fixture","monotonic":true}],'
                '"device":{"platform":"android","device_id":"serialX",'
                '"form":"physical","trust":"authorized"},'
                '"target":{"application_key":{"platform":"android",'
                '"device_id":"serialX","app_identifier":"com.example.perf",'
                '"identifier_kind":"package_name"},"runtime_state":"running",'
                '"discovery_scope":"complete_for_provider",'
                '"process_instances":[{"pid":4242,'
                '"process_start_time":"918273",'
                '"process_name":"com.example.perf","uid":10234,'
                '"is_primary":true,'
                '"ownership_evidence":"uid_and_process_name"}]},')
        f.write(f'"threads":[{{"thread_instance_id":"{THREAD}",'
                f'"process_instance_id":"{PROC}","tid":4242,"name":"main",'
                f'"is_main_ui_thread":true}}],')
        f.write('"window_start_ns":0,"window_end_ns":600000000000,"events":[')

        written = f.tell()
        i = 0
        buf = []
        while written < TARGET_BYTES:
            buf.append(
                ("," if i else "") +
                '{"event_id":"e%d","provider":"fixture.sampler",'
                '"clock_domain":"android.boottime","timestamp_ns":%d,'
                '"duration_ns":null,"process_instance_id":"%s",'
                '"thread_instance_id":"%s","category":"cpu_sample",'
                '"name":"%s"}' % (i, i * 1000, PROC, THREAD,
                                  leaves[rng.randrange(len(leaves))]))
            i += 1
            if len(buf) >= 20000:
                chunk = "".join(buf)
                f.write(chunk)
                written += len(chunk)
                buf = []
        if buf:
            chunk = "".join(buf)
            f.write(chunk)
            written += len(chunk)

        f.write('],"frames":[],"js_tasks":[],"markers":[],"cpu_samples":[],'
                '"counters":[],"refresh_intervals":[],"coverage":[],'
                '"dropped_events_by_collector":{},'
                '"build":{"facts":[],"symbol_bindings":[]}}')
    print(f"wrote {i} events to {out}")


if __name__ == "__main__":
    main()
