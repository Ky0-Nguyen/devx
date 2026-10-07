// BrowserStack: real devices in BrowserStack's cloud, from the emulator
// module, for what an emulator cannot show -- a real GPU, a real modem, a
// vendor's skin.
//
// Credentials are a BrowserStack username and access key: from the standard
// BROWSERSTACK_USERNAME / BROWSERSTACK_ACCESS_KEY variables when set,
// otherwise from the macOS Keychain (service "com.devx.browserstack"), where
// DevX's window stores them. They reach curl through a temporary owner-only
// netrc file, never through argv, which any process on the machine can read.
//
// Built against BrowserStack's published REST API (api-cloud.browserstack.com)
// and exercised against a live App Automate account (docs/browserstack.md
// says what was and was not); every call reports BrowserStack's HTTP status
// and body as they came.
#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "core/util/json.hpp"

namespace mpi::browserstack {

struct Credentials {
  std::string username;
  std::string access_key;
  std::string source;  // "environment" | "keychain"
  bool valid() const { return !username.empty() && !access_key.empty(); }
};

constexpr const char* kKeychainService = "com.devx.browserstack";

/// The environment first, then the Keychain.
std::optional<Credentials> find_credentials();

struct Reply {
  bool ok = false;      // HTTP 2xx (and JSON, unless the body is not JSON: see text)
  int http_status = 0;
  json::Value body;
  std::string text;     // the body as it came, when it was not JSON
  std::string error;
  json::Value to_json() const;
};

/// GET https://api-cloud.browserstack.com/<path>.
Reply get(const Credentials& c, const std::string& path);

/// Any request to a BrowserStack host (api-cloud for REST, hub-cloud for
/// WebDriver), or to a URL a BrowserStack reply handed back (a profiling
/// data link). Only https; the credentials are offered to BrowserStack's
/// hosts alone. `body` is JSON, sent when not empty. `raw` keeps a reply that
/// is not JSON (a CSV data file) in `Reply::text`.
Reply request(const Credentials& c, const std::string& method, const std::string& url,
              const std::string& body = {}, std::chrono::seconds timeout = std::chrono::seconds(120));
/// Uploads an .apk/.aab/.ipa for App Live (`app-live/upload`) or App Automate
/// (`app-automate/upload`). Returns BrowserStack's reply with its `app_url`.
Reply upload(const Credentials& c, const std::string& product, const std::string& file);

/// The App Live dashboard URL that opens `device` with an uploaded app
/// (`app_url` is "bs://<hash>"), for a browser.
std::string app_live_url(const std::string& os, const std::string& os_version,
                         const std::string& device, const std::string& app_url);

}  // namespace mpi::browserstack
