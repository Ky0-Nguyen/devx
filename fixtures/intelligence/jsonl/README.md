# JSONL import fixtures

**Synthetic data.** Every record here is invented; it exercises the shapes the
`jsonl` connector accepts, not any real tool's output.

- `signals.jsonl` — one full `devx.signal/1` record written by another tool
  (its `provider`, `connector_id`, `workspace_id` and `raw_ref` are replaced on
  import), loose records with `external_id` or an integer `id`, a blank line,
  two lines that claim basis `exact` without the identity to back it (a
  version alone; a 7-character SHA) and are stored as `provider_attributed`,
  and two external ids long enough that `make_signal_id` would cut them to the
  same id.
- `bad-lines.jsonl` — one readable line and four that are not imported: not
  JSON, a kind that is not a short lowercase word, no id, and an
  `occurred_at` that is not ISO 8601.
