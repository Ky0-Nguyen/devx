# DevX v0.6.1

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.1.dmg`. Signed with a Developer ID and notarized.
On 0.6.0, **Settings › Updates › Check for updates** installs it from inside
DevX.

## What changed

- **BrowserStack: a build is uploaded once, then used again.** Before, the
  uploaded app was forgotten on switching tabs or relaunching DevX, so every
  session meant uploading the file again. *Emulator › BrowserStack* now has
  an **App** panel:
  - the build in use is remembered across tabs and launches;
  - *load uploaded apps* lists what BrowserStack still holds from the last
    30 days, for App Automate and App Live, with the days left. It runs on its
    own when the panel opens. **use** picks one;
  - a `bs://` URL from CI can be pasted and used;
  - an optional *custom id* names the App Automate upload;
  - when the build in use is past BrowserStack's 30 days, the panel says to
    upload it again.

  Only the `bs://` URLs, the build's name and its upload time are stored,
  never a credential.
- *load recent builds* is now **load test runs**. It lists App Automate's
  past test runs (BrowserStack calls them builds), not app files.
