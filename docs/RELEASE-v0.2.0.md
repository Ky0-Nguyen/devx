# DevX v0.2.0

## Install

```bash
brew tap ky0-nguyen/devx https://github.com/Ky0-Nguyen/devx
brew install --cask devx
```

Or download `DevX-0.2.0.dmg`, open it, drag DevX to Applications.

**Gatekeeper will refuse it on first launch.** This build is signed ad-hoc,
not notarized. Right-click DevX in Applications and choose **Open**, once;
after that it launches normally.

- **Apple Silicon only** (arm64). macOS 14 or newer.
- `sha256` of `DevX-0.2.0.dmg`: `164ecd9d303fd9888361af72cd03093bc619d2529648f52590a2c189a2cf8c96`

## What changed

- **New bundle identifier: `com.devx`.** macOS treats this as a different app
  from v0.1.0, so preferences such as the interface language and the window
  size start fresh. Sessions in `~/.mpi/sessions` are unaffected.
- **MIT license.** The repository now has a `LICENSE`.
- **Homebrew cask.** `Casks/devx.rb` installs the app and puts `mpi` on your
  `PATH`.
- **Release pipeline.** `tools/release.sh` builds the disk image, and with a
  Developer ID identity and notary credentials it also signs with a hardened
  runtime, notarizes and staples. `.github/workflows/release.yml` runs the
  same script in CI. This release is still ad-hoc because no Developer ID is
  available yet. See `docs/packaging-and-signing.md`.
- **Fixtures renamed.** The app the real captures were recorded from now
  appears as `com.acme.shopper.debug` ("Shopper"), with placeholder backend
  hosts and username. Every measured number is unchanged.
- **Demo in the README.**

The CLI, the analysis engine and the detectors behave as in v0.1.0. The
version reported by `mpi` and the MCP server is now 0.2.0.
