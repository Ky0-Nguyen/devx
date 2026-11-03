// Whether a newer DevX has been released, and where to get it.
//
// One request, made only when someone asks for it (the window's "Check for
// updates", or a launch check they switched on): the latest release of the
// project on GitHub. It sends nothing about this Mac or its apps -- the
// request carries a User-Agent naming DevX and its version, which GitHub's
// API requires, and nothing else. The answer names the release's .dmg and its
// published SHA-256; installing is the app's job, which verifies both the
// checksum and the code signature before it replaces anything.
#pragma once

#include <string>

#include "core/net/https_fetch.hpp"
#include "core/util/json.hpp"

namespace mpi::update {

/// The repository releases are published from.
inline constexpr const char* kRepository = "Ky0-Nguyen/devx";

/// -1, 0 or 1, comparing dotted numeric versions ("0.5.10" > "0.5.9"). A
/// leading "v" is ignored; a missing component counts as 0 ("1.2" == "1.2.0").
/// Anything after the numbers ("-beta") makes a version older than the same
/// numbers without it.
int compare_versions(const std::string& a, const std::string& b);

/// Reads GitHub's "latest release" reply. `current` is this build's version.
/// Returns {ok, current, latest, newer, name, notes, published_at, html_url,
/// dmg_url, dmg_name, sha256_url} or {ok:false, error}.
json::Value parse_latest_release(const std::string& body, const std::string& current);

/// Asks GitHub for the latest release of kRepository.
json::Value check(const std::string& current, const net::Transport& transport);

}  // namespace mpi::update
