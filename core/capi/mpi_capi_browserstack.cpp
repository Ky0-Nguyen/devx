// The BrowserStack part of the C ABI (declared in mpi_capi.h).
#include <cstdlib>
#include <cstring>

#include "adapters/browserstack/browserstack.hpp"
#include "adapters/browserstack/local_tunnel.hpp"
#include "core/capi/mpi_capi.h"
#include "core/observe/observation_store.hpp"

namespace {

using namespace mpi;

char* dup_json(const json::Value& v) {
  const std::string s = v.dump(2);
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

std::string safe(const char* s) { return s == nullptr ? std::string() : std::string(s); }

json::Value no_credentials() {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(false));
  o.set("credentials", json::Value::boolean(false));
  o.set("error", json::Value::string(
                     "no BrowserStack credentials: add your username and access key"));
  return o;
}

}  // namespace

extern "C" {

char* mpi_bs_status_json(void) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  auto r = browserstack::get(*c, "app-automate/plan.json").to_json();
  r.set("credentials", json::Value::boolean(true));
  r.set("username", json::Value::string(c->username));
  r.set("source", json::Value::string(c->source));
  return dup_json(r);
}

char* mpi_bs_get_json(const char* path) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  return dup_json(browserstack::get(*c, safe(path)).to_json());
}

char* mpi_bs_upload_json(const char* product, const char* file) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  return dup_json(browserstack::upload(*c, safe(product), safe(file)).to_json());
}

char* mpi_bs_live_url_json(const char* os, const char* os_version, const char* device,
                           const char* app_url) {
  json::Value o = json::Value::object();
  o.set("url", json::Value::string(
                   browserstack::app_live_url(safe(os), safe(os_version), safe(device), safe(app_url))));
  return dup_json(o);
}

char* mpi_bs_save_json(const char* sessions_dir, const char* what, const char* json_text) {
  json::ParseError perr;
  json::Limits limits;
  limits.max_depth = 512;
  auto doc = json::parse(safe(json_text), limits, &perr);
  json::Value o = json::Value::object();
  if (!doc) {
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string("not JSON: " + perr.message));
    return dup_json(o);
  }
  json::Value summary = json::Value::object();
  summary.set("what", json::Value::string(safe(what)));
  const auto saved = observe::save_observation(safe(sessions_dir), "browserstack", safe(what), "",
                                               summary, *doc);
  o.set("ok", json::Value::boolean(saved.ok));
  if (saved.ok) o.set("id", json::Value::string(saved.id));
  else o.set("error", json::Value::string(saved.error));
  return dup_json(o);
}

char* mpi_bs_local_status_json(void) {
  json::Value o = json::Value::object();
  o.set("installed", json::Value::boolean(browserstack::local_binary_installed()));
  o.set("can_run", json::Value::boolean(browserstack::can_run_intel_binary()));
  o.set("binary", json::Value::string(browserstack::local_binary_path()));
  return dup_json(o);
}

char* mpi_bs_local_install_json(void) {
  return dup_json(browserstack::install_local_binary({}).to_json());
}

char* mpi_bs_local_start_json(const char* identifier) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  return dup_json(browserstack::start_local(*c, safe(identifier)).to_json());
}

char* mpi_bs_local_stop_json(const char* identifier) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  return dup_json(browserstack::stop_local(*c, safe(identifier)).to_json());
}

}  // extern "C"
