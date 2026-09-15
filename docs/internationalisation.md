# More than one language

DevX's interface is available in English and Vietnamese. Choose it in the
Export tab under **Language / Ngôn ngữ**, or leave it on *System*, which
follows the Mac's preferred languages.

## The catalog is keyed by the English text

`apps/devx-mac/DevX/Strings.swift` maps English sentences to their
translations. There is no `"live.panel.title"` key namespace, for two reasons.

The prose in this app *is* the product. The difference between "no issue
found" and "the detector did not run" is carried in sentences, not in labels,
and keeping the English readable at the call site — `tr("Capture
configuration")` — matters more than a tidy namespace.

And a missing translation then degrades to correct English rather than to
`live.panel.title` on screen. Falling back is not a failure state here: an
untranslated tooltip in an otherwise Vietnamese panel is a small blemish,
where a key is a bug the reader has to interpret.

The function is `tr()`, not `t()`, because this codebase already uses `t` for
loop variables. A shadowed `t` turned nine call sites into attempts to call a
JSON value, and the compiler's message for that says nothing about
translation.

## What is *not* translated, and why

Detector findings, coverage notes, threshold origins, refusal messages and
everything else the C++ core emits stay in English.

They are **evidence, not interface**. They are written into the session
package, exported, and compared across runs. Translating them would mean two
captures of the same app, taken on two machines configured differently, would
no longer be comparable — and `mpi compare` would be diffing prose rather than
measurements. The Language panel says this on screen, because a Vietnamese
reader who sees the tabs translated will otherwise read the English in the
Issues tab as an unfinished job rather than a deliberate boundary.

For the same reason these are left verbatim *inside* translated sentences:

- values the core emits — `unknown`, `not_tested`, `unsupported`, `offline`,
  `running`. A sentence explaining what `unknown` means must point at the word
  that actually appears on screen.
- command lines — `mpi analyze <session>`, `dumpsys gfxinfo framestats`,
  `xctrace record`. A translated command does not run.

## Placeholders

Strings that carry a value use named braces rather than `%@`:

```swift
tr("last looked {when}")          // "lần xem gần nhất {when}"
tr("{a} ran, {b} could not.")     // "{a} đã chạy, {b} không chạy được."
```

A translator can see what goes where, a mistyped token cannot silently swap
two values, and a translation that dropped a placeholder leaves the sentence
short rather than inserting a stray number.

## Keeping it honest: `tools/check-i18n.py`

A catalog keyed by English text cannot fail loudly. Edit a sentence in a view
and its translation silently stops applying — no compiler error, nothing on
screen to say that the Vietnamese interface just lost a paragraph. So the
check is part of the smoke test, and it reports three things:

- **stale keys** — a catalog entry whose English no longer appears in any view.
  This is an error.
- **keys that never reach `tr()`** — the text is rendered, but verbatim, so the
  translation is dead. Also an error, and it caught a real one: the first
  version of this catalog had the top-of-tab paragraph and two buttons still
  drawing in English while the checker reported success, because it only
  looked for the text and not for what was done with it.
- **coverage** — how many translatable strings have a translation. Reported,
  not enforced: English fallback is a correct answer, and failing a build over
  an untranslated tooltip would make the catalog harder to extend.

Both sides join `+`-concatenated literal runs before comparing, because the
prose here is written across several source lines and the string that reaches
the screen is the joined one.

Current state: **196 of 209** translatable strings covered. The 13 that fall
back are the `mpi …` command examples and two technical labels (`Device id`,
`Heap dump (am dumpheap)`), none of which should be translated.

## No `.lproj`

The catalog is compiled into the binary. ADR-0002 keeps this tree free of
binary assets and the bundle carries only its icon; `.lproj` directories would
be the first localisation payload and would need matching Info.plist entries.
A Swift dictionary needs neither, and `docs/packaging-and-signing.md`'s claim
that the bundle carries no localisation resources stays true.
