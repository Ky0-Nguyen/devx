# Firebase BigQuery export fixtures

**Synthetic data.** Every value here is invented: issue ids, event ids,
installation ids, timestamps, durations and the "personal data" (`Jane
Fixture`, `jane.fixture@example.com`, `user-8812`, `cart-7731-private`) that
`tests/unit/test_firebase.cpp` checks never reaches a stored signal. Only the
**shape** follows Firebase's documented BigQuery export schemas.

| File | Shape | What it pins |
|---|---|---|
| `crashlytics-batch.jsonl` | NDJSON, as `bq extract --destination_format=NEWLINE_DELIMITED_JSON` writes | FATAL / NON_FATAL / ANR, a repeated `event_id`, every timestamp spelling, an iOS row of another app with only `is_fatal` and a crashed thread, PII in `user`, `custom_keys`, `logs`, `breadcrumbs` |
| `crashlytics-query.json` | one JSON array, as `bq query --format=json` writes | a second file contributing to the same issue |
| `performance.ndjson` | NDJSON | `_app_start` with durations 1..100 ms (shuffled), a GET and a POST `NETWORK_REQUEST` on one URL pattern, a `SCREEN_TRACE` with frame ratios, a `TRACE_METRIC` |
| `malformed/truncated-array.json` | a JSON array cut off mid-element | the whole file is refused, not half-counted |
| `malformed/some-bad-lines.jsonl` | NDJSON with two unreadable lines | the readable line is imported, the sync is partial |

## Field names: verified against the official schema pages (2026-10-08)

- Crashlytics: <https://firebase.google.com/docs/crashlytics/bigquery-dataset-schema>
  — `platform` (`IOS` | `ANDROID`), `bundle_identifier`, `event_id`,
  `issue_id`, `error_type` (`FATAL`, `NON_FATAL`, `ANR`, ...), `is_fatal`
  (deprecated), `event_timestamp`, `installation_uuid`,
  `application.{display_version, build_version}`,
  `operating_system.{display_version, name, ...}`, `blame_frame.{file, line,
  symbol, library, owner, blamed, address, offset}`, `exceptions[]` (Android:
  `type`, `exception_message`, `title`, `subtitle`, `nested`, `blamed`,
  `frames[]`), `error[]` (Apple non-fatals), `threads[]` (`crashed`,
  `blamed`, `signal_name`, `signal_code`, `title`, `subtitle`, `frames[]`),
  `user.{id, name, email}`, `custom_keys[]`, `logs[]`, `breadcrumbs[]`.
  Tables are one per app (`com_example_shop_ANDROID`), with a `_REALTIME`
  twin holding the same events.
- Performance Monitoring: <https://firebase.google.com/docs/perf-mon/bigquery-export>
  — `event_timestamp`, `app_display_version`, `app_build_version`,
  `os_version`, `device_name`, `country`, `carrier`, `radio_type`,
  `custom_attributes[]`, `event_type` (`DURATION_TRACE`, `SCREEN_TRACE`,
  `TRACE_METRIC`, `NETWORK_REQUEST`), `event_name`, `parent_trace_name`,
  `trace_info.{duration_us, screen_info.{slow_frame_ratio,
  frozen_frame_ratio}, metric_info.metric_value}`,
  `network_info.{response_code, response_mime_type, request_http_method,
  request_payload_bytes, response_payload_bytes, request_completed_time_us,
  response_initiated_time_us, response_completed_time_us}`.

## Differences from what one might assume

- The Crashlytics schema has **no `issue_title` / `issue_subtitle`**. The
  connector titles an issue from the blamed exception / error / thread
  `title` and `subtitle`; a row that does carry `issue_title` (a custom view)
  is preferred.
- Performance rows carry **no app identifier** (one table per app), and a
  `NETWORK_REQUEST` row has **no `trace_info`**: its duration is
  `network_info.response_completed_time_us`.

## Assumed, not documented

- The exact JSON spelling of a `TIMESTAMP`: the pages give only the type. The
  connector accepts BigQuery's `2026-10-08 01:15:44.123456 UTC`, ISO 8601 and
  epoch numbers (s / ms / us / ns by magnitude, also as strings), and refuses
  a zone name other than UTC. `INT64` values appear as JSON strings, as
  BigQuery's JSON export writes them.
