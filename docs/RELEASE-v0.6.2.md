# DevX v0.6.2

## Install

In DevX: **Settings › Updates › Check for updates**, then **Update now and
restart**. Or:

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.2.dmg`. Signed with a Developer ID and notarized.

## What changed

- **Inspect says why a request has no body.** It used to show "not captured"
  with "the body was asked for and no answer came back", which is wrong for a
  request body: nobody asks for it, the app sends it with the request. A
  GraphQL POST without a body now shows the actual reason:
  - **the request began before DevX attached**: only its response was seen,
    so its body could not be. Start watching, then do what sends it;
  - **the app reported it without a body**: React Native and Expo include a
    request body only when it is text they can read (not a file or binary
    upload), and not before React Native 0.81 on iOS or 0.83 on Android. When
    the request carried a Content-Length, the panel says how many bytes were
    sent;
  - **React Native before 0.83 on Android** sends the name of OkHttp's body
    object (`okhttp3.RequestBody…@5f3a1c2`) in place of the body. It is no
    longer shown as if the app had posted that string;
  - a **GET** (a GraphQL persisted query, say) has no body: its data is in the
    URL.
- Checked against a real Expo 57 / React Native 0.86 app on the iOS
  simulator. A GraphQL POST sent with `fetch`, `XMLHttpRequest` (axios) or
  `FormData` carries its body to DevX in full.
