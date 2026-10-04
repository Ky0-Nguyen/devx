# DevX v0.3.0

## Install

```bash
brew tap ky0-nguyen/devx https://github.com/Ky0-Nguyen/devx
brew trust --cask ky0-nguyen/devx/devx
brew install --cask devx
```

Or download `DevX-0.3.0.dmg`, open it, drag DevX to Applications.

**This is the first notarized release.** It is signed with a Developer ID and
notarized by Apple, so DevX and `mpi` open like any other downloaded app: no
right-click > Open, no `xattr`.

- **Apple Silicon only** (arm64). macOS 14 or newer.
- `sha256` of `DevX-0.3.0.dmg`: `6ff8e9eac5b322f6d06a15e437f0cd2be3961215ad5fdd2506dfb8e414af5f93`

## What changed

- **Layout: how a screen is built.** `mpi layout` and the new Layout tab show
  views per screen, nesting depth, hidden and off-screen views, navigation
  stacks, and React Native screens mounted against showing. Nothing is added
  to the app: Android is read through `dumpsys activity top`; an iOS simulator
  through a small layout probe injected when the app is relaunched with
  `--relaunch`, which restarts it, so it is never done implicitly. A physical
  iOS device is refused. See `docs/layout.md`.
- **Results kept for AI tools.** Layout snapshots and inspect observations are
  now saved under `~/.mpi/sessions/observations/` (owner-only; `--no-save`
  skips it), next to capture sessions. `mpi mcp` gains `list_observations`,
  `read_observation` and `capture_layout`, plus `relaunch_with_layout_probe`
  behind `--allow-actions`; `observe_app` keeps what it observes. A model can
  read the whole result instead of a screenshot of it. See
  `docs/mcp-server.md`.
- **Notarized.** Signed with "Developer ID Application: Tuan Nguyen
  (9QCJJFC22X)", hardened runtime, notarized and stapled. The Homebrew cask
  no longer carries the first-launch caveat.
- **`mpi boot`** reports a tooling failure, not "no new device appeared", when
  adb or simctl never answered within the budget.
- **CI** builds the project on macOS and runs every test suite; the template
  MSBuild and SLSA workflows are gone.

## Known limits

- The React Native layout rules go by class names and have been tested
  against a labelled synthetic tree, not yet against a real React Native app.
- On Android, scroll offsets are not in `dumpsys` output, so off-screen counts
  below a scroll view are approximate; the report says so.
