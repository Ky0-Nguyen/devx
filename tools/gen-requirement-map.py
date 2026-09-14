#!/usr/bin/env python3
"""Regenerates docs/requirement-test-map.md from the built test binaries.

Each test case declares the specification checklist ids it covers; the test
framework prints them with --list-requirements. The map is therefore derived
from the tests rather than maintained by hand, so it cannot drift.

Usage:  cmake --build build && python3 tools/gen-requirement-map.py
"""
import collections
import glob
import pathlib
import os
import subprocess
import sys

REASONS_FILE = "tools/uncovered-reasons.txt"


MAP_FILE = pathlib.Path("docs/requirement-test-map.md")


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



HEADER = """# Requirement -> test mapping

Derived from the test binaries, not maintained by hand: every test case declares
the specification checklist ids it covers, and the framework prints them with
`--list-requirements`. Regenerate with `python3 tools/gen-requirement-map.py`.

**Coverage of specification section 18: {covered} of {total} checklist items \
have at
least one automated test.** The remaining {uncovered} are listed below with a \
stated
reason.

Two cautions on reading this:

1. A checklist item having a test does **not** mean the capability is verified
   on real hardware. `docs/capabilities/tested-capability-matrix.md` is the
   authority on that.
2. Several covered items are covered by a test of the *negative* guarantee
   (that the tool refuses to claim something) rather than of the positive
   behaviour.
"""


def write_map(path, mapping, checklist, covered, uncovered, extra, reasons,
              binary_count):
    """Writes the markdown map.

    This used to be missing: the script computed the coverage, printed the
    summary and wrote nothing, so a document whose own first line says it is
    not maintained by hand drifted every time a test was added.
    """
    links = sum(len(mapping[k]) for k in covered)
    tagged = {test for entries in mapping.values() for _, test in entries}

    out = [HEADER.format(covered=len(covered), total=len(checklist),
                         uncovered=len(uncovered))]

    out.append(f"## Covered ({len(covered)} items, {links} test-case links)\n")
    out.append("| Checklist id | Test binary | Test case |")
    out.append("|---|---|---|")
    for key in covered:
        for binary, test in sorted(mapping[key]):
            out.append(f"| {key} | `{binary}` | `{test}` |")
    out.append("")

    out.append("## Additional coverage keyed to specification sections and "
               f"detector ids ({len(extra)})\n")
    out.append("| Reference | Test binary | Test case |")
    out.append("|---|---|---|")
    for key in extra:
        for binary, test in sorted(mapping[key]):
            out.append(f"| {key} | `{binary}` | `{test}` |")
    out.append("")

    out.append(f"## Not yet covered ({len(uncovered)})\n")
    out.append("| Checklist id | Why not, stated |")
    out.append("|---|---|")
    for key in uncovered:
        out.append(f"| {key} | {reasons.get(key, 'NO STATED REASON')} |")
    out.append("")

    pct = round(100 * len(covered) / len(checklist))
    out.append("## Totals\n")
    out.append(f"- test binaries: {binary_count}")
    out.append(f"- test cases declaring at least one id: {len(tagged)}")
    out.append(f"- checklist-item links: {links}")
    out.append(f"- section-18 coverage: {len(covered)}/{len(checklist)} ({pct}%)")
    out.append("")

    path.write_text("\n".join(out))


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

    write_map(MAP_FILE, mapping, checklist, covered, uncovered, extra, reasons,
              len(binaries))

    print(f"covered {len(covered)}/{len(checklist)}, "
          f"uncovered {len(uncovered)}, extra refs {len(extra)}, "
          f"wrote {MAP_FILE}",
          file=sys.stderr)
    missing_reasons = [r for r in uncovered if r not in reasons]
    if missing_reasons:
        print(f"WARNING: no stated reason for: {', '.join(missing_reasons)}",
              file=sys.stderr)


if __name__ == "__main__":
    main()
