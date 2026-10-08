# DevX v0.5.1

## Install

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.5.1.dmg`. Signed with a Developer ID and notarized.

## What changed

- **Get a token, one click away.** Wherever an Intelligence connector needs a
  credential, the window has *get a token ↗*, which opens the provider's own
  page:
  - Sentry's personal tokens;
  - GitLab's token form, with the name and the `read_api` scope already filled in;
  - for a self-hosted Sentry or GitLab, the link follows your `base_url`.

  Firebase has *set it up ↗* for its BigQuery export, and the BrowserStack panel
  links to your access key. `mpi intelligence connect` prints the link and the
  Keychain command.
- **Inspect shows decoded bodies.** React Native 0.87 sends even a JSON
  response base64. When it decodes to text, Inspect shows it decoded (as a
  JSON tree where it parses) and says it did. Binary bodies stay base64.
- **AI tools can show an emulator** even when the Emulator tab has never been
  opened. Before this fix, `devx_window_show` with an AVD switched tabs and
  showed no screen.
- "credential stored" is shown only for connectors that use a credential.
- **A website**, [ky0-nguyen.github.io/devx](https://ky0-nguyen.github.io/devx/),
  in English and Vietnamese, deployed from `site/` by GitHub Pages. A change
  to the site does not release DevX.
