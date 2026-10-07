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
// and not exercised against a live account from this repository; every call
// reports BrowserStack's HTTP status and body as they came.
#pragma once

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
  bool ok = false;      // HTTP 2xx and JSON
  int http_status = 0;
  json::Value body;
  std::string error;
  json::Value to_json() const;
};

/// GET https://api-cloud.browserstack.com/<path>.
Reply get(const Credentials& c, const std::string& path);
/// Uploads an .apk/.aab/.ipa for App Live (`app-live/upload`) or App Automate
/// (`app-automate/upload`). Returns BrowserStack's reply with its `app_url`.
Reply upload(const Credentials& c, const std::string& product, const std::string& file);

/// The App Live dashboard URL that opens `device` with an uploaded app
/// (`app_url` is "bs://<hash>"), for a browser.
std::string app_live_url(const std::string& os, const std::string& os_version,
                         const std::string& device, const std::string& app_url);

}  // namespace mpi::browserstack
