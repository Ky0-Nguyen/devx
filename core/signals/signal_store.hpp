// The Local Intelligence Store (ADR-0009): the system of record for
// external evidence. Connectors refresh it; the UI, correlation and AI tools
// read it, and keep working with no network once something has been synced.
//
//   <sessions_dir>/intelligence/<workspace-id>/
//     workspace.json
//     connectors/<connector-id>.json       no secret values, ever
//     cursors/<connector-id>.json
//     signals/index.jsonl                  compact, rebuilt from the records
//     signals/<YYYY-MM-DD>/<signal-id>.json
//     raw/<provider>/<category>/<name>     immutable provider evidence
//     quarantine/                          records that failed to parse
//     state/sync-status.json, pins.json, index-dirty
//
// Everything is written to a sibling temporary file and renamed into place.
// Directories are 0700 and files 0600: raw evidence can hold personal data,
// headers and tokens a provider logged.
//
// The index is a cache. A sync marks it dirty before writing anything and
// clean after flushing it, so a sync that died halfway leaves a marker, and
// the next reader rebuilds the index from the records rather than trusting
// one that no longer matches them.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/signals/connector.hpp"
#include "core/signals/signal.hpp"

namespace mpi::signals {

std::string intelligence_root(const std::string& sessions_dir);

struct SignalQuery {
  std::string provider, connector_id, kind, severity, environment;
  std::string release_key;   // exact ReleaseIdentity::key()
  std::string version;       // matches release.version
  std::string commit;        // prefix of release.commit_sha
  std::string since, until;  // ISO 8601, on occurred_at
  std::string text;          // case-insensitive, in title / external id / kind
  std::size_t limit = 200;
  static SignalQuery from_json(const json::Value& v);
};

struct SignalQueryResult {
  /// Index entries, newest occurred_at first.
  std::vector<json::Value> entries;
  std::size_t total = 0;  // matches before the limit
  std::vector<std::string> problems;
  json::Value to_json() const;
};

struct RawRead {
  bool ok = false;
  std::string bytes;
  std::uint64_t size = 0;   // the whole file
  bool truncated = false;
  std::string error;
};

struct RetentionReport {
  bool applied = false;     // false: what would happen
  int signals_expired = 0;
  int raw_deleted = 0;
  std::uint64_t bytes_freed = 0;
  int kept_by_pin = 0;
  std::vector<std::string> notes;
  json::Value to_json() const;
};

class SignalStore {
 public:
  explicit SignalStore(std::string sessions_dir);

  // ---- workspaces ----
  std::vector<ProjectWorkspace> workspaces(std::vector<std::string>* problems = nullptr) const;
  std::optional<ProjectWorkspace> workspace(const std::string& id, std::string* error) const;
  bool save_workspace(const ProjectWorkspace& w, std::string* error);
  /// Removes the workspace and all its local evidence.
  bool delete_workspace(const std::string& id, std::string* error);

  // ---- connectors ----
  std::vector<ConnectorConfig> connectors(const std::string& ws,
                                          std::vector<std::string>* problems = nullptr) const;
  std::optional<ConnectorConfig> connector(const std::string& ws, const std::string& id,
                                           std::string* error) const;
  bool save_connector(const std::string& ws, const ConnectorConfig& c, std::string* error);
  /// Forgets the configuration and cursor. Local evidence stays unless
  /// `delete_local_data` (AC-10).
  bool remove_connector(const std::string& ws, const std::string& id, bool delete_local_data,
                        std::string* error);

  // ---- sync bookkeeping ----
  SyncCursor cursor(const std::string& ws, const std::string& connector_id) const;
  bool save_cursor(const std::string& ws, const SyncCursor& c, std::string* error);
  /// {connector_id: {last_attempt_at, last_success_at, status, error, ...}}
  json::Value sync_status(const std::string& ws) const;
  bool record_sync(const std::string& ws, const std::string& connector_id, const SyncResult& r,
                   std::string* error);

  // ---- writing evidence ----
  /// A sink for one connector's sync. Marks the index dirty; the index is
  /// flushed and marked clean when the sink is destroyed.
  std::unique_ptr<SignalSink> open_sink(const std::string& ws, const std::string& connector_id);
  bool put_signal(const std::string& ws, SignalRecord r, std::string* error);
  std::string put_raw(const std::string& ws, const std::string& provider,
                      const std::string& category, const std::string& name,
                      const std::string& bytes);

  // ---- reading evidence ----
  std::optional<SignalRecord> signal(const std::string& ws, const std::string& id,
                                     std::string* error) const;
  SignalQueryResult query(const std::string& ws, const SignalQuery& q) const;
  /// Every record, for correlation. Corrupt ones are quarantined and named.
  std::vector<SignalRecord> all_signals(const std::string& ws,
                                        std::vector<std::string>* problems = nullptr) const;
  /// At most `max_bytes` from `offset` of a raw_ref. Refuses a ref that
  /// would leave the workspace's raw directory.
  RawRead read_raw(const std::string& ws, const std::string& raw_ref, std::uint64_t offset,
                   std::size_t max_bytes) const;
  /// Lines [first, last] (1-based) of a raw_ref, read as a stream so a 20 MB
  /// log is not loaded to cut twenty lines from it; at most `max_bytes`.
  RawRead read_raw_lines(const std::string& ws, const std::string& raw_ref, std::size_t first,
                         std::size_t last, std::size_t max_bytes) const;
  bool rebuild_index(const std::string& ws, std::vector<std::string>* problems = nullptr) const;
  /// Every raw ref a stored record points at.
  std::set<std::string> referenced_raw(const std::string& ws) const;
  /// Removes older versions of a raw object that nothing references, keeping
  /// the newest. Run after every sync: a provider object that changes each
  /// time (a Sentry issue's counts) would otherwise grow without bound.
  int prune_superseded_raw(const std::string& ws);

  // ---- pins, retention, usage ----
  /// kind is "signal" or "release". A pin keeps everything it references
  /// through retention.
  bool set_pin(const std::string& ws, const std::string& kind, const std::string& id, bool pinned,
               std::string* error);
  json::Value pins(const std::string& ws) const;
  RetentionReport apply_retention(const std::string& ws, bool apply, std::int64_t now_epoch_s);
  /// Bytes and counts by provider, raw against normalized, oldest evidence.
  json::Value storage_usage(const std::string& ws) const;

  std::string workspace_dir(const std::string& ws) const;

 private:
  std::string sessions_dir_;
  friend class StoreSink;
};

}  // namespace mpi::signals
