// The JSONL import connector for DevX Intelligence (see docs/intelligence.md).
//
// The extensibility proof: any tool that can write one JSON object per line
// becomes a provider, with no change to core or to any consumer. A line is
// either a full `devx.signal/1` record or a loose object
// {kind, external_id|id, occurred_at, title, severity, environment,
//  release{...}, attributes{...}, basis}.
//
// An import cannot carry more certainty than its evidence: a line claiming
// basis "exact" without a full commit SHA, or without version + build + app
// identifier, is stored as provider_attributed, with a note.
#pragma once

#include <memory>

#include "core/signals/connector.hpp"

namespace mpi::intelligence {

std::unique_ptr<signals::SignalConnector> make_jsonl_connector();

}  // namespace mpi::intelligence
