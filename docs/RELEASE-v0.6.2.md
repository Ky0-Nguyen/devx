# DevX v0.6.2

## Install

In DevX: **Settings › Updates › Check for updates**, then **Update now and
restart**. Or:

```bash
brew update && brew upgrade --cask devx
```

Or download `DevX-0.6.2.dmg`. Signed with a Developer ID and notarized.

## What changed

- **GraphQL in Inspect, however it is sent.** Every GraphQL request goes to
  one URL, so the list now labels each row with its operation
  (`query GetMenu`, `mutation AddToCart`, `3 operations`), and the search box
  matches operation names. A **GraphQL** section in the request's detail
  shows the operation, its document with its own line breaks, and its
  variables exactly as sent. It reads:
  - a POST of `query` / `operationName` / `variables` (Apollo, urql,
    graphql-request, Relay, fetch or axios);
  - a batch (a JSON array of operations);
  - a GET with those fields in the query string, and Apollo's automatic
    persisted queries, which send only a hash (said so, with the hash);
  - a multipart upload (its `operations` part);
  - `Content-Type: application/graphql`.

  `errors` in the response is listed too. A GraphQL server reports a failed
  operation inside a 200, so the status alone said everything was fine.
  A REST call with a `query` field (`{"query":"shoes"}`) is not called
  GraphQL.
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
  - **React Native 0.85 or later** sends the 21-character placeholder
    `[Preview unavailable]` instead of a body it would not read: on iOS a
    body that is not UTF-8 text (binary, or compressed with something other
    than gzip), on Android a file or stream body it can read only once. It
    was shown as a 21-byte text body; it is now explained, with the request's
    Content-Encoding when it has one. React Native has no command to fetch
    the body itself;
  - a **GET** (a GraphQL persisted query, say) has no body: its data is in the
    URL.
- Checked against a real Expo 57 / React Native 0.86 app on the iOS
  simulator. A GraphQL POST sent with `fetch`, `XMLHttpRequest` (axios) or
  `FormData` carries its body to DevX in full.
