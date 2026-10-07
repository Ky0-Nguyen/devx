# DevX v0.4.0

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.4.0.dmg`. Signed with a Developer ID and notarized.

## What changed

- **Android emulators without Android Studio.** The new **Emulator** tab
  installs Google's emulator and system images from Google's own catalog
  (verified, with every license shown to you and accepted only by you),
  creates virtual devices from standard screen sizes, and runs them with the
  screen drawn inside DevX. Click to tap, drag to swipe, type to type. A
  toolbar gives Back, Home, Recents, rotation, volume, power, screen size and
  a screenshot, and opens the emulator's own **Extended Controls** (location,
  battery, camera, phone, fingerprint, sensors, snapshots). **Several devices
  can be shown side by side.** An existing Android SDK is reused when there
  is one. `mpi emulator` does all of it from the command line. See
  `docs/android-emulator.md`.
- **Screen sizes**: reference devices for Android's window size classes and
  real Pixel sizes, each with its size in dp and its window class. A running
  device can be overridden with no restart, or its hardware changed for the
  next (cold) boot.
- **BrowserStack** in the same tab: credentials in the Keychain, real
  devices, app upload, App Live in the browser, App Automate sessions kept
  for AI tools, and the **BrowserStack Local** tunnel so a cloud device can
  load a debug build from your Metro. Built against BrowserStack's API and not
  yet exercised against a live account. See `docs/browserstack.md`.
- **AI tools can see and operate devices** through `mpi mcp`:
  `device_screenshot` returns an image the model sees; `device_tap`,
  `device_swipe`, `device_type_text`, `device_key`, `emulator_start` and
  `emulator_stop` act with `--allow-actions`.
- **AI tools can show things in the DevX window**: `devx_window_state` and
  `devx_window_show` (a session, an issue, a layout snapshot, an emulator).
  Every time, a notice says that an AI tool did it, and why.
- The version is written in one place (`project(VERSION)`), and every push
  to `main` is versioned, built, notarized, tagged, backed up as a
  `v<version>` branch, and released.

## Known limits

- An emulator started by Android Studio (JWT auth) is listed but cannot be
  shown; start it from DevX.
- On an iOS simulator, AI tools get screenshots and layout but no input.
- BrowserStack Local is BrowserStack's Intel-only binary (Rosetta on Apple
  Silicon) and takes the access key on its command line.
- Physical devices: the Android path is the same as the emulator's but
  unverified on hardware; on a physical iPhone, capture through `xctrace`
  has not completed a recording on this host.
