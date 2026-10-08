// HTTPS requests to a provider's API, for the Intelligence connectors.
//
// Run through /usr/bin/curl with an argv and no shell (ADR-0003), like the
// BrowserStack adapter. A credential never appears in argv, which any
// process on the Mac can read: request headers that carry one go into a
// temporary owner-only curl config file (`-K`) that is deleted as soon as
// curl returns.
//
// Every request names the hosts it may reach. A URL a provider handed back
// (a pagination link) is followed only when its host is on that list, so a
// provider reply cannot send DevX -- with the token attached -- anywhere else.
//
// Connectors take a `Transport`, so tests replay recorded fixtures through
// exactly the code that parses live replies, and normal CI needs no account.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"

namespace mpi::net {

struct HttpRequest {
  std::string method = "GET";
  std::string url;                                       // https only
  /// Sent through the owner-only config file, never argv.
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;                                      // sent when not empty
  std::chrono::seconds timeout{60};
  std::size_t max_body_bytes = 32ull * 1024ull * 1024ull;
  /// Hosts this request may reach, exact (case-insensitive).
  std::vector<std::string> allowed_hosts;
  CancellationToken cancel;
};

struct HttpResponse {
  bool ok = false;           // the exchange happened (any HTTP status)
  int status = 0;
  /// Lowercased names; a repeated header keeps its last value except
  /// `link`, whose values are joined with ", " as RFC 8288 allows.
  std::map<std::string, std::string> headers;
  std::string body;
  bool truncated = false;    // body hit max_body_bytes
  std::string error;         // why the exchange did not happen

  std::optional<std::string> header(const std::string& name) const;
  bool success() const { return ok && status >= 200 && status < 300; }
};

using Transport = std::function<HttpResponse(const HttpRequest&)>;

/// The real one: curl.
HttpResponse https_fetch(const HttpRequest& req);

/// The host of an https URL, lowercased; empty for anything else.
std::string url_host(const std::string& url);

/// Whether `url` is https and its host is one of `allowed`.
bool url_allowed(const std::string& url, const std::vector<std::string>& allowed);

/// Percent-encodes everything but unreserved characters.
std::string url_encode(const std::string& s);

/// The URL of rel="next" in an RFC 8288 Link header; Sentry adds
/// `results="false"` to a next link that has nothing behind it, and that one
/// is not returned.
std::optional<std::string> next_link(const std::string& link_header);

/// Retry-After in seconds, when it is a number.
std::optional<int> retry_after_seconds(const HttpResponse& r);

/// A token for a provider, from the environment variable `env_var` first,
/// then the first of `keychain_services` that has an item (any account).
/// Returned to the caller only; nothing here writes it anywhere.
struct Secret {
  std::string value;
  std::string source;  // "environment" | "keychain"
};
std::optional<Secret> find_secret(const std::string& env_var,
                                  const std::vector<std::string>& keychain_services);
/// Whether find_secret would find one, without reading its value -- so
/// asking (for a health badge) never makes macOS ask for Keychain access.
bool secret_present(const std::string& env_var, const std::vector<std::string>& keychain_services);

}  // namespace mpi::net
