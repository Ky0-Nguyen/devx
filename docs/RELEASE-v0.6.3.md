# DevX v0.6.3

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.3.dmg`. Signed with a Developer ID and notarized.
On 0.6.0 or later, **Settings › Updates › Check for updates** installs it
from inside DevX.

## What changed

- **Inspect: React Native's `[Preview unavailable]` is no longer shown as a
  request body.** React Native 0.85 and later send that 21-character
  placeholder to the debugger in place of a body they will not read:
  - on iOS, a body that is not UTF-8 text (binary, or compressed with
    something other than gzip, which React Native decompresses itself);
  - on Android, a file or stream body that can be read only once, or one
    that failed to read.

  DevX showed it as a 21-byte text body, which read as the app having posted
  those characters, or as DevX having cut the body short. The request body
  section now says *withheld by React Native* and explains why, naming the
  request's `Content-Encoding` when it has one. React Native offers no
  command to fetch the body itself, so DevX cannot show it.

  A JSON string sent with `fetch`, axios or Apollo is neither case: when a
  GraphQL request shows this, something in the app turned the body into
  bytes before sending it.
