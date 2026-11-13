# DevX v0.6.4

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.4.dmg`. Signed with a Developer ID and notarized.
On 0.6.0 or later, **Settings › Updates › Check for updates** installs it
from inside DevX.

## What changed

- **Inspect a release build.** A release build runs no inspector, so until
  now Inspect had nothing to attach to and showed nothing. The in-app SDK has
  a new `trackNetwork()`: it wraps the app's own `fetch` and
  `XMLHttpRequest` and sends each request to DevX while DevX listens.
  *Inspect* has a new source, **release build · SDK**, beside
  **debug build · Metro**, and shows these requests exactly as it shows a
  debug build's:
  - method, URL, status, duration, failures;
  - request and response headers;
  - request and response bodies (text, up to 512 KB each);
  - GraphQL operation names, documents, variables, and `errors` inside a
    200;
  - search, copy as cURL.

  The panel shows the endpoint and token to build into the app, the snippet
  to paste, and the `adb reverse` line for Android. The port and token stay
  the same between launches, because a release build has them built in. The
  iOS simulator needs nothing extra; a physical iPhone cannot reach the
  listener, which takes connections from the Mac only.

  `trackNetwork()` sends headers and bodies verbatim, Authorization included,
  so it belongs in a build made for inspecting and never in a store build.
  It records nothing until DevX is listening, and `stop()` puts `fetch` and
  `XMLHttpRequest` back. Network only: console and Redux still need a debug
  build. See `sdk/react-native/README.md`.
- **`mpi sdk-network`** does the same from the command line:
  `mpi sdk-network --port 47081 --token <token>` listens, and prints the
  report, in `mpi inspect --detail`'s shape, when stopped.
