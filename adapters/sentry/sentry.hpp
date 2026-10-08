// The Sentry connector for DevX Intelligence (see docs/intelligence.md).
//
// Reads one Sentry project through Sentry's public REST API (`/api/0/`):
// its releases, its issues, and the latest event of the most recently active
// issues. Everything goes through ConnectorContext::transport, with the auth
// token in an Authorization header only, to the configured host only.
#pragma once

#include <memory>
#include <string>

#include "core/signals/connector.hpp"

namespace mpi::intelligence {

std::unique_ptr<signals::SignalConnector> make_sentry_connector();

/// A Sentry release string as a ReleaseIdentity, by Sentry's convention
/// `package@version+build`: "com.acme.app@5.4.0+54019" is bundle, version and
/// build. A string without a package is a version and nothing more ("5.4.0",
/// and "5.4.0+77" too: without the package the convention does not apply, so
/// no build is read into it), except a full 40- or 64-character hex SHA,
/// which is a commit. The source is FactSource::kProvider when anything was
/// read, and the result is empty for an empty string.
signals::ReleaseIdentity parse_sentry_release(const std::string& release);

}  // namespace mpi::intelligence
