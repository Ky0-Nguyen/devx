#!/usr/bin/env python3
"""Regenerates docs/requirement-test-map.md from the built test binaries.

Each test case declares the specification checklist ids it covers; the test
framework prints them with --list-requirements. The map is therefore derived
from the tests rather than maintained by hand, so it cannot drift.

Usage:  cmake --build build && python3 tools/gen-requirement-map.py
"""
import collections
import glob
import os
import subprocess
import sys

REASONS_FILE = "tools/uncovered-reasons.txt"


def load_reasons():
    reasons = {}
    if not os.path.exists(REASONS_FILE):
        return reasons
    for line in open(REASONS_FILE):
        line = line.strip()
        if not line or line.startswith("#") or "\t" not in line:
            continue
        req, why = line.split("\t", 1)
        reasons[req.strip()] = why.strip()
    return reasons


def main():
    os.environ["MPI_FIXTURE_DIR"] = os.path.abspath("fixtures")
    binaries = sorted(glob.glob("build/bin/test_*"))
    if not binaries:
        sys.exit("no test binaries found; run `cmake --build build` first")

    mapping = collections.defaultdict(list)
    for binary in binaries:
        out = subprocess.run([binary, "--list-requirements"],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "\t" not in line:
                continue
            req, test = line.split("\t", 1)
            mapping[req.strip()].append((os.path.basename(binary), test.strip()))

    # Section 18 of the specification, by prefix and count.
    sections = [("A", 25), ("B", 15), ("C", 20), ("D", 22), ("E", 22),
                ("F", 17), ("G", 19), ("H", 17), ("I", 21), ("J", 20)]
    checklist = {f"{p}{n:02d}" for p, c in sections for n in range(1, c + 1)}

    covered = sorted(k for k in mapping if k in checklist)
    uncovered = sorted(checklist - set(mapping))
    extra = sorted(k for k in mapping if k not in checklist)
    reasons = load_reasons()

    print(f"covered {len(covered)}/{len(checklist)}, "
          f"uncovered {len(uncovered)}, extra refs {len(extra)}",
          file=sys.stderr)
    missing_reasons = [r for r in uncovered if r not in reasons]
    if missing_reasons:
        print(f"WARNING: no stated reason for: {', '.join(missing_reasons)}",
              file=sys.stderr)


if __name__ == "__main__":
    main()
