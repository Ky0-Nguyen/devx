# DevX v0.5.3

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.5.3.dmg`. Signed with a Developer ID and notarized.

## What changed

Pressing a button now always shows that something happened.

- **"use" says what it did and what comes next.** Choosing a device or an app
  (in Devices, Apps, Recent targets or Inspect) shows a bar under the toolbar,
  for example *✓ App selected: com.example.app — next: Preflight · Live ·
  Record · Inspect · Layout*, with buttons to those screens. The bar clears
  itself after 20 seconds or once you go to one of them. The chosen row's
  button reads **✓ in use**. In Inspect the next step is on the same screen,
  so the bar says so: choose what to capture, then *watch live*.
- **Everything in flight is listed at the foot of the sidebar**, on every tab,
  with a spinner and a count. This includes the work the busy overlay does
  not cover: anything started from Live, and the BrowserStack and
  Intelligence panels.
- **BrowserStack buttons show their own progress.** *load devices*, *upload
  app…*, *start session*, *start tunnel*, *download*, *sessions*, *keep for AI
  tools*, *import profiling*, *save to Keychain* and *stop* each turn into a
  spinner with "loading…", "uploading…", "starting…" while they run, and
  cannot be pressed twice. An upload says when both copies are in, and what
  to do next.
- **Intelligence**: *save workspace* and *add connector* show progress and
  then confirm, with the next step. The header spinner no longer stops while
  other reads are still running.
- **Live**: *Open in Issues* shows "opening…" (the busy overlay is not shown
  on Live). **Inspect**: *observe for N s* says "observing…" only while it is
  observing, not while the attachable apps are being listed. **Help**: *check
  Claude Code* shows that it is checking.
