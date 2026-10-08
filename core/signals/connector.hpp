// The connector contract (ADR-0010).
//
// Every provider -- Sentry, GitLab, Firebase, a JSONL import, whatever comes
// next -- implements SignalConnector and hands what it found to a SignalSink.
// The sink is the persistence boundary: nothing a connector fetched is part
// of the project's evidence until the sink has written it, and the UI,
// correlation and AI tools read the store, never a connector. A new provider
// therefore changes no consumer.
//
// Connectors do their I/O through ConnectorContext::transport, so a test
// drives a real connector against recorded replies.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/net/https_fetch.hpp"
#include "core/signals/signal.hpp"
#include "core/util/cancel.hpp"

namespace mpi::signals {

struct ConnectorInfo {
  std::string provider;      // sentry
  std::string display_name;  // Sentry
  std::string category;      // production | ci_cd | testing | import
  /// What it sends out and to where, for the egress ledger (section 18.1).
  std::string egress;
  /// Where its credential comes from, in words: "SENTRY_AUTH_TOKEN or the
  /// Keychain item com.devx.sentry". Empty for a connector that needs none.
  std::string credential_help;
  /// The standard environment variable and Keychain service of its
  /// credential; both empty for a connector that needs none.
  std::string credential_env;
  std::string credential_service;
  /// True when it also works without one (a public GitLab project); the
  /// connector then runs with no secret and says what it could not reach.
  bool credential_optional = false;
  /// Where a person gets that credential: the provider's own page, opened
  /// from the window or printed by the CLI. `{base_url}` is replaced with the
  /// connector's base_url setting, or `default_base_url`, so a self-hosted
  /// Sentry or GitLab links to its own page.
  std::string credential_url;
  std::string default_base_url;
  /// Where to set up what a connector without a credential reads (Firebase's
  /// BigQuery export, say), or its documentation.
  std::string setup_url;
  /// The settings it understands, name -> description.
  std::vector<std::pair<std::string, std::string>> settings;
  json::Value to_json() const;
};

struct ConnectorCapabilities {
  bool issues = false;
  bool crashes = false;
  bool metrics = false;
  bool traces = false;
  bool ci_pipelines = false;
  bool ci_jobs = false;
  bool deployments = false;
  bool tests = false;
  bool artifacts = false;
  bool webhooks = false;
  bool incremental_pull = false;
  bool events = false;  // generic events (an import)
  bool network = true;  // false: reads local files only
  json::Value to_json() const;
};

struct ValidateResult {
  bool ok = false;
  std::string error;
  std::string account;       // who the credential belongs to, when known
  std::vector<std::string> notes;
  json::Value to_json() const;
};

struct DiscoverResult {
  bool ok = false;
  std::string error;
  /// A partial list says so here, rather than reading as a failure.
  std::vector<std::string> notes;
  /// What could be synced: [{id, name, kind}], e.g. Sentry projects.
  json::Value resources = json::Value::array();
  json::Value to_json() const;
};

struct ConnectorHealth {
  /// connected | needs_auth | error | partial | syncing | up_to_date | never_synced | paused
  std::string state;
  std::string detail;
  json::Value to_json() const;
};

/// Where a connector puts what it found. Implemented by the store.
class SignalSink {
 public:
  virtual ~SignalSink() = default;
  /// Keeps provider bytes as they came; returns the local raw_ref
  /// ("raw/<provider>/<category>/<name>"), or empty on failure. Writing the
  /// same bytes again returns the same ref; different bytes under the same
  /// name get a new versioned ref, so evidence once referenced never changes.
  virtual std::string put_raw(const std::string& provider, const std::string& category,
                              const std::string& name, const std::string& bytes) = 0;
  /// Stores or updates one normalized record. False with a reason when the
  /// record is not valid.
  virtual bool put_signal(SignalRecord record, std::string* error) = 0;
  /// The record already stored under `id`, if any: for a connector that
  /// re-reads an object without everything it knew last time (an event it
  /// did not fetch again) and must carry that forward rather than erase it.
  virtual std::optional<SignalRecord> existing(const std::string& id) {
    (void)id;
    return std::nullopt;
  }
};

/// What a connector runs with.
struct ConnectorContext {
  net::Transport transport;           // net::https_fetch in production
  /// The credential, resolved by the caller from ConnectorConfig and never
  /// stored. Empty for a connector that needs none.
  std::optional<net::Secret> secret;
  CancellationToken cancel;
  std::string workspace_id;
  std::string now_iso;                // observed_at for every record of this sync
  /// Bounds, so one sync cannot run away.
  int max_pages = 20;
  int max_records = 2000;
  /// Per raw object (one log, one export file), not per sync; the store
  /// refuses anything over 64 MB regardless.
  std::size_t max_raw_bytes = 20ull * 1024ull * 1024ull;
};

class SignalConnector {
 public:
  virtual ~SignalConnector() = default;
  virtual ConnectorInfo info() const = 0;
  virtual ConnectorCapabilities capabilities() const = 0;
  virtual ValidateResult validate(const ConnectorConfig& config, const ConnectorContext& ctx) = 0;
  virtual DiscoverResult discover(const ConnectorConfig& config, const ConnectorContext& ctx) = 0;
  /// Fetches what changed since `cursor` and writes it to `sink`. Returns a
  /// partial result -- records kept, cursor not advanced past them -- when it
  /// stops early for any reason.
  virtual SyncResult sync(const ConnectorConfig& config, const SyncCursor& cursor,
                          SignalSink& sink, const ConnectorContext& ctx) = 0;
};

using ConnectorFactory = std::function<std::unique_ptr<SignalConnector>()>;

/// The providers this build knows. Adapters register theirs at startup
/// (adapters/intelligence/builtin.cpp); core never names a provider.
void register_connector(const std::string& provider, ConnectorFactory factory);
std::unique_ptr<SignalConnector> make_connector(const std::string& provider);
std::vector<std::string> registered_providers();

/// Where a configured connector's credential is read from. Names, never
/// values. A credential belongs to one connector, so it can never be sent to
/// another connector's host:
///  - `credential_ref`, when set, is the only Keychain service used;
///  - otherwise the connector's own item, `<service>.<connector-id>`
///    (com.devx.sentry.sentry-prod), then the provider's shared item for
///    what `mpi` users saved before connectors had their own;
///  - the provider's environment variable only when this is the workspace's
///    one connector of that provider (`env_allowed`): with two GitLab
///    connectors, GITLAB_TOKEN would otherwise go to both hosts.
struct CredentialSource {
  std::string env_var;
  std::vector<std::string> keychain_services;
  bool needed() const { return !env_var.empty() || !keychain_services.empty(); }
};
CredentialSource credential_source(const ConnectorConfig& config, const ConnectorInfo& info,
                                   bool env_allowed);

/// `info.credential_url` for one connector's settings; empty when there is
/// none. Only ever an https URL.
std::string credential_url_for(const ConnectorInfo& info, const json::Value& settings);

}  // namespace mpi::signals
