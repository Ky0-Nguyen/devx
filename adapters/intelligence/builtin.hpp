// The connectors this build ships, registered with the core registry.
#pragma once

namespace mpi::intelligence {

/// Registers Sentry, GitLab, Firebase and JSONL import. Safe to call more
/// than once; every front end calls it before touching Intelligence.
void register_builtin_connectors();

}  // namespace mpi::intelligence
