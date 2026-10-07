# BrowserStack: real devices beside the emulators

The Emulator tab's **BrowserStack** panel reaches real devices in
BrowserStack's cloud with your own account, for what an emulator cannot show:
a real GPU, a real modem, a vendor's skin. Minutes are BrowserStack's to bill.

**Status:** this is built against BrowserStack's published REST API
(`api-cloud.browserstack.com`) and has **not been exercised against a live
account** from this repository. Every call reports BrowserStack's HTTP status
and body as they came, so a mismatch shows itself rather than passing
silently.

## Credentials

Your username and access key, from BrowserStack's Account > Settings:

- `BROWSERSTACK_USERNAME` and `BROWSERSTACK_ACCESS_KEY`, when set, take
  precedence;
- otherwise the macOS **Keychain**, service `com.devx.browserstack`. The
  panel's *save to Keychain* writes it through the Security framework.
  `mpi` and `mpi mcp` read it with `/usr/bin/security`, and macOS may ask
  once whether they may.

DevX passes them to `curl` in a temporary netrc file that only you can read,
and deletes it when `curl` returns. They are never written into a command
line, which any process on the Mac can read.

## What the panel does

- **Account**: checks the credentials against the App Automate plan.
- **Devices**: lists BrowserStack's real devices (`app-automate/devices.json`).
- **Upload app**: uploads an `.apk`, `.aab` or `.ipa` for App Live and shows
  the `bs://` app URL.
- **Open in App Live**: opens the App Live dashboard in your browser with the
  chosen device and the uploaded app. App Live is BrowserStack's interactive
  stream, and it runs in their web page; DevX cannot embed it.
- **App Automate sessions**: recent builds, and *keep for AI tools* saves a
  build's sessions (status, device, video and log URLs) as an observation
  `mpi mcp` can read.

## BrowserStack Local

BrowserStack Local is BrowserStack's tunnel. It lets their devices reach your
Mac: `localhost`, a staging server on your network, or **Metro**. On the
device, your Mac is `bs-local.com`. Point a React Native debug build at
`bs-local.com:8081` and it loads its bundle from your Metro. DevX's
**Inspect** tab then reads its network, console and Redux through that same
Metro, as it does for a local device.

- The binary is BrowserStack's own, about 10 MB, downloaded when you press
  *download* into `~/Library/Application Support/DevX/browserstack`.
- BrowserStack publishes it **for Intel Macs only**. On Apple Silicon it runs
  under **Rosetta 2**. DevX checks for Rosetta and, where it is missing, says
  so and does not install it (`softwareupdate --install-rosetta` is your
  decision).
- It takes the **access key on its command line** (`--key`). That is
  BrowserStack's design, and while the tunnel runs other processes on this Mac
  can see the key.
- Each DevX window uses its own `local-identifier` (`devx-<pid>`), so two
  tunnels on one account do not collide.

## What it cannot do

A BrowserStack device gives no adb or shell access, so **Live, Record and
Layout do not work on it**. Inspect does, through BrowserStack Local and
Metro, as described above.

## For AI tools

`mpi mcp` offers `browserstack_devices`, `browserstack_sessions` (which also
keeps what it read as an observation) and `browserstack_upload`. The last one
sends a file to a third party, so it needs `--allow-actions`.
