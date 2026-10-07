# DevX v0.4.1

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.4.1.dmg`. Signed with a Developer ID and notarized.
Homebrew offers it once the release's pull request (version and cask) is
merged into `main`.

0.4.0 was never published: its release stopped at `main`'s branch
protection. Everything in [the 0.4.0 notes](RELEASE-v0.4.0.md) (Android
emulators inside DevX, BrowserStack, AI tools that operate devices and show
things in the window) ships for the first time in this release.

## What changed

- **Real BrowserStack devices, driven from DevX.** *Start session* runs your
  app on a real device in BrowserStack's cloud with BrowserStack's settings:
  network profile, GPS, timezone, language and locale, orientation,
  biometric and camera image injection, App Profiling, and routing through
  BrowserStack Local. The screen shows in the panel (a screenshot every
  1.5 s). Click to tap, drag to swipe, type, and Back, Home and Enter.
  `mpi browserstack start | screenshot | tap | swipe | type | key | stop`
  does the same from a terminal, and `browserstack_session_*` tools let AI
  tools do it with `--allow-actions`.
- **BrowserStack App Profiling as a DevX session.** CPU, threads, memory,
  battery, disk and network I/O, fps and slow frames, measured by
  BrowserStack, become counters on the Timeline and in `mpi mcp`.
  You can import them on stop, or later with *import profiling* /
  `mpi browserstack import <session>`. Measured on a Google Pixel 9
  (Android 16). The numbers are BrowserStack's, and the session says so.
- **BrowserStack is now tested against a live account**: status, devices,
  upload, sessions, input and profiling. Its series format, which
  BrowserStack does not document, is read as BrowserStack actually serves it.
- **`mpi inspect` against current Metro.** React Native 0.86+ and Expo's dev
  server refuse a debugger without a matching `Origin`. DevX now sends the one
  they accept. Measured on a React Native CLI 0.87 debug app: network,
  console, and Redux with action names.
- **No more silent exits.** A peer that drops a connection made `mpi` die of
  `SIGPIPE` with nothing printed (exit 141). Every socket now turns that into
  an error the report states.
- **Expo Go is named for what it is**: a runtime with no JavaScript debugger.
  The report says this instead of "the runtime refused the watcher".
- A response size React Native does not know is shown as `-`, not `-1`.
- **Releases go through pull requests.** CI no longer pushes to `main`. It
  tags the release, keeps the `v<version>` backup branch, publishes the
  GitHub release, and opens a pull request that brings the version and the
  Homebrew cask to `main`.

## Known limits

- A BrowserStack session's screen is a screenshot every 1.5 s, not a video
  stream. Live, Record and Layout need adb and do not work on BrowserStack
  devices; their performance comes from App Profiling.
- BrowserStack lists App Profiling under paid plans. It worked on the Free
  plan this was tested with, which may be a trial allowance.
- iOS on BrowserStack, App Live and BrowserStack Local were not exercised
  against the live account.
- The limits listed for 0.4.0 still apply.
