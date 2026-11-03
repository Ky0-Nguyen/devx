# DevX v0.6.0

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.0.dmg`. Signed with a Developer ID and notarized.
From this version on, DevX can also update itself: **Settings › Updates ›
Check for updates**.

## What changed

- **Update from inside DevX.** *Settings › Updates* (also *DevX › Check for
  Updates…*) asks GitHub for the latest release, shows its notes, and offers
  **Update now and restart**. How it updates depends on how DevX was
  installed:
  - installed with **Homebrew**: it runs `brew update` and
    `brew upgrade --cask devx`, so Homebrew's record and the `mpi` link stay
    right;
  - installed from the **disk image**:
    - it downloads the release's `.dmg` and checks it against the
      published SHA-256;
    - it checks that the app inside is signed by DevX's Developer ID
      (team 9QCJJFC22X) and is the version the release names;
    - only then does it replace this copy, and restart. If any step fails,
      nothing is changed;
  - a folder DevX cannot write to: the checked disk image is opened in Finder
    to drag across;
  - a development build is never replaced; it says to use git.
- **Nothing leaves the Mac unless you ask.** The check is one request to
  `api.github.com` naming DevX and its version, listed in *What leaves this
  machine*. *Check when DevX opens* is off by default; switched on, it checks
  at most once a day and announces a new version in the confirmation bar.
- Updating refuses to restart DevX while a live capture or an Inspect watch is
  running.
