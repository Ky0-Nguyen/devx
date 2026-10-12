# DevX v0.5.2

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.5.2.dmg`. Signed with a Developer ID and notarized.

## What changed

- **A guided tour on first launch**, in English and Vietnamese. One card per
  tab -- all seventeen -- each opening its screen behind it and highlighting
  it in the sidebar, with what the tab is for and the first thing to try. The
  first card picks the language; the last one shows where to choose features.
  Replay it from Help, Settings or *File › Replay the tour*, or open it with
  `open -a DevX --args --tour=<card>`.
- **Choose which features the sidebar shows.** *Settings › Features* lists
  every tab by group (set up, capture, analyze, connect, the app), with a description in
  both languages and a switch. The main features are on by default; Preflight,
  Layout, Threads, Compare and Detectors are one click away. *Main features
  only* and *Show all* reset the list. The tour card for each tab has the same
  switch.
- **DevX remembers.** The chosen features, and the tab you left, are restored
  on the next launch. A tab added by a later version takes its default instead
  of reading as one you turned off. A launch that names a tab or a session
  still wins.
- **A hidden tab is never lost.** `--tab`, a session link or an AI tool can
  still open it, and it stays in the sidebar while it is open. Settings and
  Help are always shown.
- The Export tab is now **Settings**: it holds the feature switches, the
  language, export, paths and the egress ledger. `--tab=settings` is unchanged.
