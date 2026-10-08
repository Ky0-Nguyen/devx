// The signal plane: long-lived evidence from production, CI/CD, releases and
// external tools (ADR-0008).
//
// A measurement session is a window of time measured on one device, and
// NormalizedTrace is its contract. A signal is something a provider reported
// -- a Sentry issue, a GitLab job, a Firebase metric -- that lives for weeks
// and belongs to a release rather than to a window. The two planes meet only
// in correlation (core/correlation), so neither weakens the other's
// semantics.
//
// Conservative by construction, as everywhere else (ADR-0005): an absent
// field is absent, never an empty string standing in for "none"; a release
// whose build number nobody reported has no build number; and every link says
// what it rests on (EvidenceBasis), so "candidate" can never be read as
// "cause".
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::signals {

inline constexpr const char* kSignalSchema = "devx.signal/1";
inline constexpr const char* kWorkspaceSchema = "devx.workspace/1";
inline constexpr const char* kConnectorSchema = "devx.connector/1";

/// What a fact or a link rests on.
enum class EvidenceBasis {
  /// The same deterministic identity on both sides: a full commit SHA, or
  /// version + build + app identifier agreeing.
  kExact,
  /// The provider itself declares it (a Sentry event names its release).
  kProviderAttributed,
  /// A heuristic: timing, a short SHA prefix, a version without its build.
  /// Never a cause.
  kCandidate,
  /// No defensible link.
  kUnknown,
};
const char* to_string(EvidenceBasis b);
EvidenceBasis basis_from_string(const std::string& s);

/// Where a release fact came from.
enum class FactSource { kProvider, kBuildManifest, kGit, kUserAsserted, kDeviceProvider, kUnknown };
const char* to_string(FactSource s);
FactSource fact_source_from_string(const std::string& s);

/// Which build of which app, as far as anyone said.
struct ReleaseIdentity {
  std::optional<std::string> version;
  std::optional<std::string> build_number;
  std::optional<std::string> commit_sha;   // lowercase hex, as reported
  std::optional<std::string> branch;
  std::optional<std::string> environment;
  std::optional<std::string> bundle_or_package_id;
  FactSource source = FactSource::kUnknown;

  bool empty() const;
  json::Value to_json() const;
  static ReleaseIdentity from_json(const json::Value& v);

  /// The key a release is filed under: "<app>@<version>+<build>" when a
  /// version is known (parts that are unknown are left out), otherwise
  /// "commit:<sha>", otherwise empty -- a signal with no identity belongs to
  /// no release, rather than to one invented for it.
  std::string key() const;
};

/// One piece of external evidence, normalized.
struct SignalRecord {
  std::string schema_version = kSignalSchema;
  std::string id;             // stable: sig_<connector>_<external id>
  std::string workspace_id;
  std::string provider;       // sentry | gitlab | firebase | jsonl | ...
  std::string connector_id;   // one configured connection
  /// crash | issue | metric | pipeline | ci_job | deploy | test | release | event
  std::string kind;
  std::string external_id;
  std::string occurred_at;    // ISO 8601 as the provider gave it
  std::string observed_at;    // when DevX stored it
  std::optional<std::string> environment;
  /// fatal | error | warning | info, or the provider's own word
  std::optional<std::string> severity;
  std::optional<std::string> title;
  ReleaseIdentity release;
  json::Value attributes = json::Value::object();
  std::string raw_ref;        // local, immutable provider evidence; may be empty
  EvidenceBasis basis = EvidenceBasis::kUnknown;  // of `release`

  json::Value to_json() const;
  /// Nullopt, with the reason, for a record that is not a valid signal --
  /// a corrupt file is reported, never half-read.
  static std::optional<SignalRecord> from_json(const json::Value& v, std::string* error);
};

/// A stable signal id from its connector and the provider's id: the same
/// provider object always lands on the same file, so a re-sync updates it.
/// At most 64 characters; a longer one ends in a hash of the full pair, so
/// two ids never collide by truncation.
std::string make_signal_id(const std::string& connector_id, const std::string& external_id);

/// How long evidence is kept.
struct RetentionPolicy {
  /// 0 = forever.
  int days = 30;
  json::Value to_json() const;
  static RetentionPolicy from_json(const json::Value& v);
};

/// A project: a repository and the apps built from it.
struct ProjectWorkspace {
  std::string id;
  std::string name;
  std::string repository_root;               // may be empty: no code context
  std::vector<std::string> app_identifiers;  // package names, bundle ids
  std::vector<std::string> environments;
  RetentionPolicy retention;
  json::Value to_json() const;
  static std::optional<ProjectWorkspace> from_json(const json::Value& v, std::string* error);
};

/// One configured connection to a provider. Never holds a secret: the
/// credential is looked up at sync time from the environment or the Keychain
/// (`credential_ref` names which), and only that name is stored.
struct ConnectorConfig {
  std::string id;              // e.g. sentry-main
  std::string provider;        // sentry | gitlab | firebase | jsonl
  std::string name;
  /// Provider settings: base URL, organization, project, import path... Any
  /// key that looks like it holds a secret is refused when saved.
  json::Value settings = json::Value::object();
  std::string credential_ref;  // Keychain service / account, or env var name
  std::optional<RetentionPolicy> retention;  // overrides the workspace's
  bool paused = false;
  json::Value to_json() const;
  static std::optional<ConnectorConfig> from_json(const json::Value& v, std::string* error);
};

/// Where a connector got to.
struct SyncCursor {
  std::string connector_id;
  /// The newest provider time fully synced; the next sync asks for what
  /// changed after it. Advanced only by a complete sync.
  std::string watermark;
  /// A provider page cursor to resume a partial sync from.
  std::string resume;
  std::string updated_at;
  json::Value extra = json::Value::object();  // provider-specific bookkeeping
  json::Value to_json() const;
  static SyncCursor from_json(const json::Value& v);
};

enum class SyncStatus { kComplete, kPartial, kFailed };
const char* to_string(SyncStatus s);

/// What one sync did. A partial sync says so, and its records are kept;
/// the cache must never look more current than it is.
struct SyncResult {
  SyncStatus status = SyncStatus::kFailed;
  int records_written = 0;
  int raw_written = 0;
  int pages = 0;
  bool rate_limited = false;
  std::optional<int> retry_after_s;
  std::string error;
  std::vector<std::string> notes;
  SyncCursor cursor;           // to persist
  std::string started_at, finished_at;
  json::Value to_json() const;
};

/// Whether a settings key would hold a secret: token, password, secret, key
/// (but not "project_key"-style identifiers), dsn.
bool key_looks_secret(const std::string& key);

/// Lowercase hex of 7..64 characters.
bool looks_like_sha(const std::string& s);

/// A safe id: [a-z0-9_-], 1..64, not starting with '-'.
bool id_is_safe(const std::string& id);

/// Seconds since the epoch from ISO 8601 ("2026-10-08T01:15:44Z",
/// "...44.123+07:00", "2026-10-08"); nullopt for anything else. Fractions are
/// dropped.
std::optional<std::int64_t> parse_iso8601(const std::string& s);

/// The reverse, in UTC with a Z.
std::string format_iso8601(std::int64_t epoch_s);

}  // namespace mpi::signals
