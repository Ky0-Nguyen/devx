// What an AI agent is given (section 16): bounded, cited, redacted.
//
// The model reasons; DevX does everything deterministic first -- joins,
// percentiles, finding the failing line in a 20 MB log, resolving a stack
// frame to a file at the release's commit -- and hands over a pack of small
// facts with the local evidence ids they came from. Raw evidence leaves the
// store only as a bounded excerpt that has been through the redactor, and
// only when asked for; a pack never contains a provider credential, because
// none is ever stored.
#pragma once

#include <string>

#include "core/correlation/engine.hpp"
#include "core/signals/signal_store.hpp"

namespace mpi::correlation {

/// Hard bounds on what any one answer carries.
inline constexpr std::size_t kMaxExcerptBytes = 8 * 1024;
inline constexpr std::size_t kMaxPackBytes = 64 * 1024;

/// One signal: the normalized record first, then -- with `include_raw` -- a
/// bounded, redacted excerpt of its raw evidence. For a CI job the excerpt
/// is the failure window the connector located, not the head of the log.
json::Value signal_read(const signals::SignalStore& store, const std::string& ws,
                        const std::string& signal_id, bool include_raw);

/// Deterministic code context for a signal: the release's commit, its stack
/// frames resolved to files in the workspace's repository, the lines around
/// each at that commit (HEAD, said so, when the commit is not here), blame,
/// and the release's diff limited to those files.
json::Value code_context_for_signal(const signals::SignalStore& store, const Graph& g,
                                    const std::string& ws, const std::string& signal_id);

struct PackScope {
  std::string release;    // a release key, or
  std::string signal_id;  // one signal (its release is added)
  std::string question;   // what the person asked, passed through
  bool include_raw = true;
  bool include_code = true;
};

/// {question_scope, freshness, facts, exact_links, candidate_links,
///  raw_excerpts, code_context, missing_evidence, source_refs}, within
/// kMaxPackBytes, redacted.
json::Value evidence_pack(const signals::SignalStore& store, const Graph& g, const std::string& ws,
                          const PackScope& scope, const json::Value& freshness);

}  // namespace mpi::correlation
