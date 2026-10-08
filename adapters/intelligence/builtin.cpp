#include "adapters/intelligence/builtin.hpp"

#include <mutex>

#include "adapters/firebase/firebase.hpp"
#include "adapters/gitlab/gitlab.hpp"
#include "adapters/jsonl/jsonl.hpp"
#include "adapters/sentry/sentry.hpp"

namespace mpi::intelligence {

void register_builtin_connectors() {
  static std::once_flag once;
  std::call_once(once, [] {
    signals::register_connector("sentry", make_sentry_connector);
    signals::register_connector("gitlab", make_gitlab_connector);
    signals::register_connector("firebase", make_firebase_connector);
    signals::register_connector("jsonl", make_jsonl_connector);
  });
}

}  // namespace mpi::intelligence
