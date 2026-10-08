// Intelligence, as one set of JSON-in / JSON-out operations.
//
// The window (through the C ABI), `mpi intelligence` and `mpi mcp` all call
// these, so a sync, a release overview or an evidence pack is the same
// answer wherever it was asked for. Every answer is an object with "ok";
// a failure carries "error" and changes nothing.
#pragma once

#include <string>

#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::intelligence {

json::Value providers();
json::Value egress(const std::string& sessions_dir);

json::Value workspaces(const std::string& sessions_dir);
/// {id, name, repository_root, app_identifiers, environments, retention}
json::Value workspace_save(const std::string& sessions_dir, const json::Value& workspace);
json::Value workspace_delete(const std::string& sessions_dir, const std::string& ws);

json::Value integrations(const std::string& sessions_dir, const std::string& ws);
/// {id, provider, name, settings, credential_ref, retention, paused}
json::Value connector_save(const std::string& sessions_dir, const std::string& ws,
                           const json::Value& connector);
json::Value connector_remove(const std::string& sessions_dir, const std::string& ws,
                             const std::string& connector_id, bool delete_local_data);
json::Value connector_validate(const std::string& sessions_dir, const std::string& ws,
                               const std::string& connector_id, const CancellationToken& cancel);
json::Value connector_discover(const std::string& sessions_dir, const std::string& ws,
                               const std::string& connector_id, const CancellationToken& cancel);
/// connector_id empty: every connector of the workspace that is not paused.
json::Value sync(const std::string& sessions_dir, const std::string& ws,
                 const std::string& connector_id, const CancellationToken& cancel);

json::Value overview(const std::string& sessions_dir, const std::string& ws);
json::Value signals_query(const std::string& sessions_dir, const std::string& ws, const json::Value& query);
json::Value signal_read(const std::string& sessions_dir, const std::string& ws, const std::string& id,
                        bool include_raw);
json::Value releases(const std::string& sessions_dir, const std::string& ws);
json::Value release(const std::string& sessions_dir, const std::string& ws, const std::string& key);
json::Value compare(const std::string& sessions_dir, const std::string& ws, const std::string& base,
                    const std::string& candidate);
json::Value related(const std::string& sessions_dir, const std::string& ws, const std::string& signal_id);
json::Value code_context(const std::string& sessions_dir, const std::string& ws,
                         const std::string& signal_id);
/// {release?, signal_id?, question?, include_raw?, include_code?}
json::Value evidence_pack(const std::string& sessions_dir, const std::string& ws,
                          const json::Value& scope);

json::Value retention(const std::string& sessions_dir, const std::string& ws, bool apply);
json::Value pin(const std::string& sessions_dir, const std::string& ws, const std::string& kind,
                const std::string& id, bool pinned);
json::Value link_session(const std::string& sessions_dir, const std::string& ws,
                         const std::string& session_id, const std::string& release_key, bool linked);

/// The workspace to use when none was named: the only one, or an error
/// listing them.
std::string default_workspace(const std::string& sessions_dir, std::string* error);

}  // namespace mpi::intelligence
