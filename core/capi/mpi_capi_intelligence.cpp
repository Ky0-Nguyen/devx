// The Intelligence part of the C ABI (declared in mpi_capi.h): thin wrappers
// over adapters/intelligence/service, so the window gets exactly what
// `mpi intelligence` and `mpi mcp` get. SwiftUI never talks to a provider
// (FR-17); it calls these.
#include <cstdlib>
#include <cstring>

#include "adapters/intelligence/service.hpp"
#include "core/capi/mpi_capi.h"

namespace mpi::capi {
CancellationToken current_cancel_token();
}  // namespace mpi::capi

namespace {

using namespace mpi;

char* dup_json(const json::Value& v) {
  const std::string s = v.dump(2);
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

std::string safe(const char* s) { return s == nullptr ? std::string() : std::string(s); }

json::Value parse_or_empty(const char* text) {
  json::ParseError perr;
  auto v = json::parse(safe(text), json::Limits{}, &perr);
  return v && v->is_object() ? *v : json::Value::object();
}

}  // namespace

extern "C" {

char* mpi_intelligence_providers_json(void) { return dup_json(intelligence::providers()); }

char* mpi_intelligence_egress_json(const char* sessions_dir) {
  return dup_json(intelligence::egress(safe(sessions_dir)));
}

char* mpi_intelligence_workspaces_json(const char* sessions_dir) {
  return dup_json(intelligence::workspaces(safe(sessions_dir)));
}

char* mpi_intelligence_workspace_save_json(const char* sessions_dir, const char* workspace_json) {
  return dup_json(intelligence::workspace_save(safe(sessions_dir), parse_or_empty(workspace_json)));
}

char* mpi_intelligence_workspace_delete_json(const char* sessions_dir, const char* workspace_id) {
  return dup_json(intelligence::workspace_delete(safe(sessions_dir), safe(workspace_id)));
}

char* mpi_intelligence_integrations_json(const char* sessions_dir, const char* workspace_id) {
  return dup_json(intelligence::integrations(safe(sessions_dir), safe(workspace_id)));
}

char* mpi_intelligence_connector_save_json(const char* sessions_dir, const char* workspace_id,
                                           const char* connector_json) {
  return dup_json(intelligence::connector_save(safe(sessions_dir), safe(workspace_id),
                                               parse_or_empty(connector_json)));
}

char* mpi_intelligence_connector_remove_json(const char* sessions_dir, const char* workspace_id,
                                             const char* connector_id, int delete_local_data) {
  return dup_json(intelligence::connector_remove(safe(sessions_dir), safe(workspace_id),
                                                 safe(connector_id), delete_local_data != 0));
}

char* mpi_intelligence_validate_json(const char* sessions_dir, const char* workspace_id,
                                     const char* connector_id) {
  return dup_json(intelligence::connector_validate(safe(sessions_dir), safe(workspace_id),
                                                   safe(connector_id), capi::current_cancel_token()));
}

char* mpi_intelligence_discover_json(const char* sessions_dir, const char* workspace_id,
                                     const char* connector_id) {
  return dup_json(intelligence::connector_discover(safe(sessions_dir), safe(workspace_id),
                                                   safe(connector_id), capi::current_cancel_token()));
}

char* mpi_intelligence_sync_json(const char* sessions_dir, const char* workspace_id,
                                 const char* connector_id) {
  return dup_json(intelligence::sync(safe(sessions_dir), safe(workspace_id), safe(connector_id),
                                     capi::current_cancel_token()));
}

char* mpi_intelligence_overview_json(const char* sessions_dir, const char* workspace_id) {
  return dup_json(intelligence::overview(safe(sessions_dir), safe(workspace_id)));
}

char* mpi_intelligence_signals_json(const char* sessions_dir, const char* workspace_id,
                                    const char* query_json) {
  return dup_json(intelligence::signals_query(safe(sessions_dir), safe(workspace_id),
                                              parse_or_empty(query_json)));
}

char* mpi_intelligence_signal_json(const char* sessions_dir, const char* workspace_id,
                                   const char* signal_id, int include_raw) {
  return dup_json(intelligence::signal_read(safe(sessions_dir), safe(workspace_id), safe(signal_id),
                                            include_raw != 0));
}

char* mpi_intelligence_releases_json(const char* sessions_dir, const char* workspace_id) {
  return dup_json(intelligence::releases(safe(sessions_dir), safe(workspace_id)));
}

char* mpi_intelligence_release_json(const char* sessions_dir, const char* workspace_id,
                                    const char* release_key) {
  return dup_json(intelligence::release(safe(sessions_dir), safe(workspace_id), safe(release_key)));
}

char* mpi_intelligence_compare_json(const char* sessions_dir, const char* workspace_id,
                                    const char* base_release, const char* candidate_release) {
  return dup_json(intelligence::compare(safe(sessions_dir), safe(workspace_id), safe(base_release),
                                        safe(candidate_release)));
}

char* mpi_intelligence_related_json(const char* sessions_dir, const char* workspace_id,
                                    const char* signal_id) {
  return dup_json(intelligence::related(safe(sessions_dir), safe(workspace_id), safe(signal_id)));
}

char* mpi_intelligence_code_context_json(const char* sessions_dir, const char* workspace_id,
                                         const char* signal_id) {
  return dup_json(intelligence::code_context(safe(sessions_dir), safe(workspace_id), safe(signal_id)));
}

char* mpi_intelligence_evidence_pack_json(const char* sessions_dir, const char* workspace_id,
                                          const char* scope_json) {
  return dup_json(intelligence::evidence_pack(safe(sessions_dir), safe(workspace_id),
                                              parse_or_empty(scope_json)));
}

char* mpi_intelligence_retention_json(const char* sessions_dir, const char* workspace_id, int apply) {
  return dup_json(intelligence::retention(safe(sessions_dir), safe(workspace_id), apply != 0));
}

char* mpi_intelligence_pin_json(const char* sessions_dir, const char* workspace_id, const char* kind,
                                const char* id, int pinned) {
  return dup_json(intelligence::pin(safe(sessions_dir), safe(workspace_id), safe(kind), safe(id),
                                    pinned != 0));
}

char* mpi_intelligence_link_session_json(const char* sessions_dir, const char* workspace_id,
                                         const char* session_id, const char* release_key, int linked) {
  return dup_json(intelligence::link_session(safe(sessions_dir), safe(workspace_id), safe(session_id),
                                             safe(release_key), linked != 0));
}

}  // extern "C"
