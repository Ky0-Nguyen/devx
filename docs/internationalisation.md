# More than one language

DevX's interface is available in English and Vietnamese. Choose it in the
DevX's interface is available in English and Vietnamese. Choose it from the **Language** menu, from the small picker at the foot of the sidebar, or in the Export tab's **Language** panel, or leave it on *System*, which follows the Mac's preferred languages. The control was only in the Export tab at first, and someone looking for it could not find it: a global preference behind a tab named after something else., or leave it on *System*, which
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

## Core text: translated for display, English on disk

The original decision here was that core-emitted text stayed English
everywhere, and the reasoning below explains why. That reasoning was about the
**stored** text, and it still holds -- but it was applied too widely, and the
result was a Vietnamese interface whose Detectors and Issues tabs read in
English.

So there are two catalogs. `Strings.vietnamese` is this app's own chrome;
`Strings.vietnameseCore` is text the analysis core emits, translated at the
moment it is drawn. Nothing about what is written to disk changes: the session
package, the JSON and Markdown exports, and everything `mpi compare` reads
stay in one language, so two captures taken on differently configured machines
remain comparable.

Identifiers are not in either catalog and must not be:
`frames_responsiveness`, `configurable_heuristic`, `DET-01`, `frame_records`.
Those are values, not prose -- they appear in the exported document, in
`--suppressions` files and in the specification, and a translated identifier
matches none of them.

`tools/check-i18n.py` verifies the core catalog against the **C++** sources
rather than the Swift views, because that text never appears in a `.swift`
file. Checking it the same way as the UI catalog reported all 110 entries as
dead.

## What is *still* not translated, and why

Detector findings, coverage notes, threshold origins, refusal messages and
What the C++ core emits stays in English **on disk**: the session package, the JSON and Markdown exports, and everything `mpi compare` reads. On screen, `Strings.vietnameseCore` translates detector titles, evidence requirements, threshold origins and caveats at the moment they are drawn; what still reaches the screen in English is listed below..

The stored text is **evidence, not interface**. It is written into the session package, exported, and compared across runs, so it is never translated: two captures of the same app taken on two machines configured differently must stay comparable, and `mpi compare` must diff measurements rather than prose. That rule is about what is on disk; the display lookup does not touch it. They are written into the session
package, exported, and compared across runs. Translating them would mean two
captures of the same app, taken on two machines configured differently, would
no longer be comparable — and `mpi compare` would be diffing prose rather than
measurements. The Language panel still carries a note saying this text stays in English (`SettingsView.swift`, `languagePanel`). It was written before the core catalog existed and now describes the on-disk rule as if it applied on screen; the header comment at the top of `Strings.swift` says the same. Both are out of date and should say what this section says., because a Vietnamese
reader who sees the tabs translated will otherwise read the English in the
Issues tab as an unfinished job rather than a deliberate boundary.

For the same reason these are left verbatim *inside* translated sentences:

- values the core emits — `unknown`, `not_tested`, `unsupported`, `offline`,
  `running`. A sentence explaining what `unknown` means must point at the word
  that actually appears on screen.
- command lines — `mpi record --heap`, `mpi analyze --suppressions`. A translated command does not run. The `mpi …` hints under empty states (`mpi analyze <session>`, `mpi compare <baseline> <candidate>`) are whole strings rather than words inside a sentence; they have no catalog entry and fall back.,
  `xctrace record`. A translated command does not run.

- technical terms the reader already uses in English -- React Native's thread names `UI`, `JS` and `native` inside the Threads labels, and the Inspect filters `API`, `Redux`, `Log`, `Console`, `Request`. Translating them would make the labels harder to match against the documentation, not easier. The filter names reach `tr()` with no catalog entry and fall back on purpose. The catalog's own comment about the thread names is why the checker drops whole-line comments before reading it: the quoted tokens were once read as a catalog entry and reported as a dead translation.

## Placeholders

Strings that carry a value use named braces rather than `%@`:

```swift
tr("last looked {when}")          // "lần xem gần nhất {when}"
tr("{a} ran, {b} could not.")     // "{a} đã chạy, {b} không chạy được."
```

A translator can see what goes where, a mistyped token cannot silently swap
two values, and a translation that dropped a placeholder leaves the sentence
short rather than inserting a stray number.

Interpolating a Swift value inside the literal -- `tr("\(result.hidden) request(s) are hidden by the filter")` -- produces a different string on every render, so no catalog key can match it and the sentence falls back to English on every machine. Six such sentences went in with the device and request filters (`Views.swift`, `InspectView.swift`). The form that works is already in the device list: `DeviceFreshness.fill(tr("{n} hidden by the filter"), "{n}", count)` looks the template up first and substitutes afterwards. The checker cannot tell this case from an ordinary untranslated string; it shows in `--coverage` as a fall-back whose text begins with `\(`.

## Keeping it honest: `tools/check-i18n.py`

A catalog keyed by English text cannot fail loudly. Edit a sentence in a view
and its translation silently stops applying — no compiler error, nothing on
screen to say that the Vietnamese interface just lost a paragraph. So the
check is part of the smoke test, and it reports five things:

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

Current state: **369 of 398** translatable UI strings covered, and all 110 core translations still emitted.
The 29 that fall back are of three kinds. Six `mpi …` hints under empty states and a dozen technical labels -- `Device id`, `Heap dump (am dumpheap)`, the Inspect filters `API`, `Redux`, `Log`, `Console`, `Request`, the Live counters `tick`, `ticks`, `stacks`, `payload:`, the search field's `url, action, status…` -- should not be translated. Six are sentences that interpolate a Swift value inside `tr()` -- "\(result.hidden) request(s) are hidden by the filter…" and its siblings for lines and devices, the two Ambiguous-target banners, `observe for \(n)s` -- which no catalog key can ever match; they should use the `{placeholder}` form above. Five are not strings a user sees: a `DispatchQueue` label, picked up because `label:` marks a translating call, and four fragments cut short where an interpolation's inner quote ends the checker's literal regex. (`Device id`,

- **translations identical to their English, or empty** -- almost always a forgotten entry rather than a word that happens to be the same. An error, in both catalogs.
- **core keys no longer in any C++ source** -- the core catalog's stale-key check, run against `core/` and `adapters/` (`.cpp`, `.hpp`) rather than the views, since that text never appears in a `.swift` file. An error.

`--quiet` prints nothing on success, which is how the smoke test calls it. `--coverage` prints the strings that fall back, one per line, and nothing else; the numbers below are read off that list.

## No `.lproj`

The catalog is compiled into the binary. This tree carries no binary asset a reviewer cannot read -- the icon itself is drawn at build time by `tools/gen-icon.swift` for that reason, see `docs/packaging-and-signing.md` -- and the bundle carries only that icon; `.lproj` directories would be the first localisation payload and would need matching Info.plist entries.
binary assets and the bundle carries only its icon; `.lproj` directories would
be the first localisation payload and would need matching Info.plist entries.
A Swift dictionary needs neither, and `docs/packaging-and-signing.md`'s claim
that the bundle carries no localisation resources stays true.
