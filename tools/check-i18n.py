#!/usr/bin/env python3
"""Checks the translation catalog against the strings the app actually renders.

The catalog in Strings.swift is keyed by the English text itself, which makes a
missing translation degrade to correct English -- and makes a *stale* key
invisible. Change a sentence in a view and its translation silently stops
applying, with no compiler error and nothing on screen to show that the
Vietnamese interface just lost a paragraph. This is the check for that.

Both sides join `+`-concatenated literal runs before comparing, because the
prose in this app is written across several source lines and the string that
reaches the screen is the joined one.

Usage: tools/check-i18n.py [--quiet]
Exit status 0 if every catalog key is still rendered somewhere, 1 otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "devx-mac", "DevX")
CATALOG = os.path.join(SRC, "Strings.swift")

# A run of one or more string literals joined by `+`.
RUN = re.compile(r'"(?:[^"\\\n]|\\.)*"(?:\s*\+\s*\n?\s*"(?:[^"\\\n]|\\.)*")*')
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')



def strip_comment_lines(text):
    """Drops whole-line `//` comments.

    Necessary because the catalog's own comments quote the tokens they are
    about -- a note explaining that "UI" and "JS" stay in English put three
    bare literals into the file, which the pair walker read as a catalog entry
    and then reported as a dead translation. Only whole-line comments are
    removed: a `//` inside a string is part of a URL, not a comment.
    """
    out = []
    for line in text.split("\n"):
        out.append("" if line.lstrip().startswith("//") else line)
    return "\n".join(out)

def unescape(s):
    return (s.replace('\\"', '"').replace("\\n", "\n")
             .replace("\\t", "\t").replace("\\\\", "\\"))


def joined_runs(text):
    """Every concatenated literal run in `text`, joined and unescaped."""
    out = set()
    for m in RUN.finditer(text):
        parts = LIT.findall(m.group(0))
        if parts:
            out.add(unescape("".join(parts)))
    return out


# Call sites that translate their argument, either directly or because the
# component does it: Panel/Banner/BulletList and the empty-state view all run
# `t()` over these parameters (see Theme.swift).
TRANSLATING = ("tr(", "title:", "subtitle:", "message:", "detail:", "hint:",
               "items:", "label:")
# Call sites that render their argument as-is.
VERBATIM = ("Text(", "Button(", "navigationTitle(", "Label(", "help(",
            "placeholder:")


def unreached(keys):
    """Keys rendered somewhere without passing through a translating call."""
    sources = {}
    for f in sorted(glob.glob(os.path.join(SRC, "*.swift"))):
        if os.path.basename(f) == "Strings.swift":
            continue
        sources[os.path.basename(f)] = strip_comment_lines(
            open(f, encoding="utf-8").read())

    bad = []
    for key in keys:
        reached = False
        seen_at = None
        for name, text in sources.items():
            for m in RUN.finditer(text):
                if unescape("".join(LIT.findall(m.group(0)))) != key:
                    continue
                # Look back over the statement for the nearest marker. The
                # nearest one wins: `Text(t("x"))` is translated and
                # `Text("x")` is not, and they differ only in what sits
                # immediately before the literal.
                before = text[max(0, m.start() - 160):m.start()]
                best, kind = -1, None
                for token in TRANSLATING:
                    i = before.rfind(token)
                    if i > best:
                        best, kind = i, "ok"
                for token in VERBATIM:
                    i = before.rfind(token)
                    if i > best:
                        best, kind = i, "verbatim"
                if kind == "ok":
                    reached = True
                elif seen_at is None:
                    seen_at = name
        if not reached:
            bad.append((key, seen_at or "?"))
    return bad


CORE_DIRS = ("core", "adapters")


def cpp_strings():
    """Every string literal in the C++ sources, joined across adjacent parts.

    The core catalog is verified against these rather than against the Swift
    views, because that text arrives at runtime from the analysis core -- it
    never appears in a `.swift` file, and checking it the same way reported
    all 110 entries as dead.
    """
    out = set()
    for d in CORE_DIRS:
        for root, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for name in files:
                if not name.endswith((".cpp", ".hpp")):
                    continue
                text = open(os.path.join(root, name), encoding="utf-8",
                            errors="replace").read()
                # C++ concatenates adjacent literals with no operator, so the
                # run regex needs no `+` between them.
                for m in re.finditer(
                        r'"(?:[^"\\\n]|\\.)*"(?:\s*\n?\s*"(?:[^"\\\n]|\\.)*")*',
                        text):
                    parts = LIT.findall(m.group(0))
                    if parts:
                        out.add(unescape("".join(parts)))
    return out


def catalog_pairs(which="vietnamese"):
    """The catalog's (english, translation) pairs, both sides joined."""
    src = strip_comment_lines(open(CATALOG, encoding="utf-8").read())
    start = src.index("static let " + which + ":")
    body = src[start:]
    # Stop at the next `static let`, so the two catalogs do not bleed.
    nxt = body.find("static let ", 12)
    if nxt != -1:
        body = body[:nxt]
    # Entries are `<run> : <run> ,` -- walk runs in order and pair them up.
    runs = []
    for m in RUN.finditer(body):
        parts = LIT.findall(m.group(0))
        runs.append((unescape("".join(parts)), m.end()))
    pairs, i = [], 0
    while i + 1 < len(runs):
        # A pair is two runs separated by a colon; anything else (a stray
        # literal in a comment, say) would desynchronise the walk, so the
        # separator is checked rather than assumed.
        between = body[runs[i][1]:runs[i + 1][1]]
        if ":" in between.split('"')[0]:
            pairs.append((runs[i][0], runs[i + 1][0]))
            i += 2
        else:
            i += 1
    return pairs


def translation_targets():
    """Every string the views hand to `tr()`, explicitly or via a component.

    This is the other direction from the staleness check, and the one the
    screen makes obvious: a string that reaches `tr()` with no catalog entry
    falls back to English, correctly but visibly, in the middle of an
    otherwise translated panel.
    """
    out = set()
    for f in sorted(glob.glob(os.path.join(SRC, "*.swift"))):
        if os.path.basename(f) == "Strings.swift":
            continue
        text = strip_comment_lines(open(f, encoding="utf-8").read())
        for m in re.finditer(r'\btr\(', text):
            run = RUN.match(text, m.end())
            if not run:
                continue    # tr(title), tr(item) -- a variable, not a literal
            out.add(unescape("".join(LIT.findall(run.group(0)))))
        # Titles and subtitles handed straight to a translating component.
        for label in ("title:", "subtitle:", "message:", "detail:", "hint:",
                      "label:"):
            for m in re.finditer(re.escape(label) + r'\s*\n?\s*', text):
                run = RUN.match(text, m.end())
                if run:
                    out.add(unescape("".join(LIT.findall(run.group(0)))))
    return out


def main():
    quiet = "--quiet" in sys.argv
    rendered = set()
    for f in sorted(glob.glob(os.path.join(SRC, "*.swift"))):
        if os.path.basename(f) == "Strings.swift":
            continue
        rendered |= joined_runs(
            strip_comment_lines(open(f, encoding="utf-8").read()))

    core_pairs = catalog_pairs("vietnameseCore")
    core_available = cpp_strings()
    core_stale = [en for en, _ in core_pairs if en not in core_available]
    core_unchanged = [en for en, vi in core_pairs if en == vi]

    pairs = catalog_pairs("vietnamese")
    if not pairs:
        print("check-i18n: could not read the catalog at all", file=sys.stderr)
        return 1

    stale = [en for en, _ in pairs if en not in rendered]
    # Appearing in a source file is not enough: the string has to actually
    # reach `t()`. The first version of this check passed a catalog whose
    # top-of-tab paragraph and buttons were still rendering in English,
    # because it only looked for the text and not for what was done with it.
    untranslated = unreached([en for en, _ in pairs if en in rendered])
    # A translation identical to its English is almost always a forgotten
    # entry rather than a word that happens to be the same.
    unchanged = [en for en, vi in pairs if en == vi]
    empty = [en for en, vi in pairs if not vi.strip()]

    ok = True
    if core_stale:
        ok = False
        print(f"check-i18n: {len(core_stale)} core catalog key(s) no longer "
              f"appear in any C++ source -- their translations are dead:",
              file=sys.stderr)
        for s2 in core_stale:
            print(f"  - {s2[:110]!r}", file=sys.stderr)
    if core_unchanged:
        ok = False
        print(f"check-i18n: {len(core_unchanged)} core translation(s) "
              f"identical to the English:", file=sys.stderr)
        for s2 in core_unchanged:
            print(f"  - {s2[:110]!r}", file=sys.stderr)
    if untranslated:
        ok = False
        print(f"check-i18n: {len(untranslated)} catalog key(s) are rendered "
              f"without passing through tr(), so the translation never "
              f"applies:", file=sys.stderr)
        for s2, where in untranslated:
            print(f"  - {where}: {s2[:90]!r}", file=sys.stderr)
    if stale:
        ok = False
        print(f"check-i18n: {len(stale)} catalog key(s) are no longer rendered "
              f"anywhere -- their translations are dead:", file=sys.stderr)
        for s in stale:
            print(f"  - {s[:110]!r}", file=sys.stderr)
    for label, group in (("identical to the English", unchanged),
                         ("empty", empty)):
        if group:
            ok = False
            print(f"check-i18n: {len(group)} translation(s) {label}:",
                  file=sys.stderr)
            for s in group:
                print(f"  - {s[:110]!r}", file=sys.stderr)
    # Coverage is reported, not enforced: English fallback is a correct
    # answer, and failing the build over an untranslated tooltip would make
    # the catalog harder to extend rather than easier.
    have = {en for en, _ in pairs}
    missing = sorted(t2 for t2 in translation_targets() if t2 and t2 not in have)
    if "--coverage" in sys.argv:
        for m2 in missing:
            print(m2)
        return 0 if ok else 1
    if ok and not quiet:
        targets = len(translation_targets())
        done = targets - len(missing)
        print(f"check-i18n: {len(pairs)} UI translations, all still "
              f"rendered; {done} of {targets} translatable UI strings covered "
              f"({len(missing)} fall back to English). "
              f"{len(core_pairs)} core translations, all still emitted.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
