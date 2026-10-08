// Running a configured connector: the one path every front end uses -- the
// window, `mpi intelligence sync` and the MCP `connector_sync` tool -- so a
// sync behaves the same wherever it was asked for.
//
// The credential is resolved here, from the environment or the Keychain, held
// in memory for the duration of the call, and handed to the connector only.
#pragma once

#include <string>

#include "core/signals/connector.hpp"
#include "core/signals/signal_store.hpp"

namespace mpi::signals {

struct RunOptions {
  net::Transport transport;  // empty: net::https_fetch
  CancellationToken cancel;
  /// For tests: a credential to use instead of looking one up.
  std::optional<net::Secret> secret_override;
};

/// Validate, discover, sync: each answers {"ok": ..., ...} as JSON.
json::Value validate_connector(SignalStore& store, const std::string& ws,
                               const std::string& connector_id, const RunOptions& opts);
json::Value discover_connector(SignalStore& store, const std::string& ws,
                               const std::string& connector_id, const RunOptions& opts);
/// Syncs into the store and records the outcome. The JSON is
/// SyncResult::to_json() plus "connector_id".
json::Value sync_connector(SignalStore& store, const std::string& ws,
                           const std::string& connector_id, const RunOptions& opts);

/// Every connector of a workspace with its configuration (never a secret),
/// capabilities, health and freshness -- what Integrations shows.
json::Value integrations(const SignalStore& store, const std::string& ws);

/// Per connector: state (complete/partial/failed/never_synced), last
/// success and last attempt -- from the store alone, no credential lookup,
/// so it is cheap enough to attach to every answer.
json::Value freshness(const SignalStore& store, const std::string& ws);

/// The providers this build can configure, with their settings and egress.
json::Value providers();

/// What can leave this machine, connector by connector (section 18.1).
json::Value egress_ledger(const SignalStore& store);

}  // namespace mpi::signals
