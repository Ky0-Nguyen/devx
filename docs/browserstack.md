# BrowserStack: real devices beside the emulators

The Emulator tab's **BrowserStack** panel reaches real devices in
BrowserStack's cloud with your own account, for what an emulator cannot show:
a real GPU, a real modem, a vendor's skin. Minutes are BrowserStack's to bill.

**Status:** exercised against a live account (App Automate, Free plan):
status, devices, upload, a real-device session on a Google Pixel 9
(Android 16) with taps, swipes, Home and Back, and App Profiling imported
from it. Every call reports BrowserStack's HTTP status and body as they came,
so a mismatch shows itself rather than passing silently. App Live and the
BrowserStack Local tunnel were not exercised against the live account.

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
- **Upload app**: uploads an `.apk`, `.aab` or `.ipa` twice, once for App Live
  and once for App Automate (BrowserStack keeps them apart), and shows the
  `bs://` app URL.
- **Open in App Live**: opens the App Live dashboard in your browser with the
  chosen device and the uploaded app. App Live is BrowserStack's interactive
  stream, and it runs in their web page; DevX cannot embed it.
- **App Automate sessions**: recent builds and their sessions. *Keep for AI
  tools* saves a build's sessions (status, device, video and log URLs) as an
  observation `mpi mcp` can read. *Import profiling* brings one session's App
  Profiling into DevX (below).

## A real-device session DevX drives

*Start session* on a device runs your app on a real BrowserStack device and
shows it in the panel. Under the hood this is an App Automate (Appium)
session that DevX opens on `hub-cloud.browserstack.com`:

- **Settings**, set in *Session settings* before starting: network profile
  (`4g-lte-good`, `3g-umts-lossy`, `no-network`, ...), GPS location,
  timezone, language and locale, orientation, biometric injection, camera
  image injection, App Profiling, and routing through BrowserStack Local.
  A setting left empty is not sent, so BrowserStack's default holds.
- **The screen** is a screenshot every 1.5 s, not a video stream: WebDriver
  offers no stream. Click to tap, drag to swipe, type into whatever has
  focus, and Back, Home and Enter. Coordinates are the screenshot's pixels;
  DevX maps them to the session's units (iOS counts points).
- **Stop** ends the session. With App Profiling on, DevX then waits for
  BrowserStack to publish the profiling (it took about 20 s) and opens it on
  the Timeline.
- Minutes are billed by BrowserStack while the session runs. DevX asks
  BrowserStack to end it after 300 s without a command, and a session that
  ended that way still stops cleanly.

The same session from a terminal:

```bash
mpi browserstack upload app-release.apk                      # bs://...
mpi browserstack start bs://<hash> "Google Pixel 9" 16.0 --network 4g-lte-good \
    --gps 35.6762,139.6503 --timezone Tokyo --profiling       # prints the session id
mpi browserstack screenshot <session> --out screen.png
mpi browserstack tap <session> 540 218
mpi browserstack swipe <session> 540 1800 540 900
mpi browserstack key <session> home
mpi browserstack stop <session> --import-profiling
```

The session lives on BrowserStack's side, so the window, `mpi browserstack`
and `mpi mcp` can all drive the same one by its id.

## App Profiling, imported

BrowserStack measures an app it runs with `appProfiling` on: CPU, memory,
battery, disk and network I/O, and frame rate, sampled about once a second.
DevX imports that as an ordinary DevX session, so the Timeline, Issues,
Compare and `mpi mcp` read it like a capture made on the desk:

```bash
mpi browserstack import <session>      # or: stop <session> --import-profiling
```

| BrowserStack field | DevX counter | unit |
|---|---|---|
| `cpu_usage`, `cpu_threads` | `cpu.usage_percent`, `process.threads` | %, count |
| `mem_usage` | `memory.used_bytes` | MB, converted to bytes |
| `batt_usage` | `battery.used_mah` | mAh, cumulative |
| `reads`, `writes` | `disk.read_kb`, `disk.write_kb` | kB per sample |
| `uploads`, `downloads` | `network.upload_kb`, `network.download_kb` | kB per sample |
| `fps`, `slow_fps` | `ui.fps`, `ui.slow_frames` | fps, count |

- The measurements are **BrowserStack's**, not DevX's. Every counter names
  its source, the device is described as BrowserStack reported it, and the
  session says it was imported.
- BrowserStack documents its summary but not the series behind each
  `*_data` link. The shape above is the one it served (samples with
  `time_offset_ms` from the session's start), and a field DevX does not know
  is kept under its own name rather than dropped. The whole reply is also
  kept as an observation.
- The Timeline counts BrowserStack's span, from first sample to last, as
  measured, and nothing outside it. A bin shows its last reading, so a
  short burst of disk or network I/O can be hidden by the zero after it in a
  wide bin; more bins (`mpi timeline --bins 60`) show it.
- BrowserStack lists App Profiling under its paid plans. It worked on the
  Free plan this was tested with, which may be a trial allowance.

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
Layout do not work on it**. Its performance comes from App Profiling
instead. Inspect works through BrowserStack Local and Metro, as described
above. The session's screen is a screenshot every 1.5 s, so an animation
cannot be watched in it.

## For AI tools

`mpi mcp` offers:

- read-only: `browserstack_devices`, `browserstack_sessions` (which also
  keeps what it read as an observation), `browserstack_import_profiling`,
  and `browserstack_session_screenshot` (the screen as an image);
- actions, with `--allow-actions`: `browserstack_upload` (sends a file to a
  third party), `browserstack_session_start`, `browserstack_session_input`
  (tap, swipe, type, key) and `browserstack_session_stop` (with
  `import_profiling`). Starting a session spends BrowserStack minutes.
