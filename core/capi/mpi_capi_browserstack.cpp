// The BrowserStack part of the C ABI (declared in mpi_capi.h).
#include <cstdlib>
#include <cstring>

#include <fstream>

#include "adapters/browserstack/automate.hpp"
#include "adapters/browserstack/browserstack.hpp"
#include "adapters/browserstack/local_tunnel.hpp"
#include "adapters/browserstack/profiling.hpp"
#include "core/capi/mpi_capi.h"
#include "core/observe/observation_store.hpp"

namespace mpi::capi {
CancellationToken current_cancel_token();
}  // namespace mpi::capi

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

char* mpi_bs_import_profiling_json(const char* sessions_dir, const char* build_id,
                                   const char* session_id) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  browserstack::ProfilingRequest req;
  req.sessions_dir = safe(sessions_dir);
  req.build_id = safe(build_id);
  req.session_id = safe(session_id);
  return dup_json(
      browserstack::import_profiling(*c, req, capi::current_cancel_token()).to_json());
}

char* mpi_bs_automate_start_json(const char* spec_json) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  const auto spec = json::parse(safe(spec_json), nullptr);
  if (!spec || !spec->is_object()) {
    json::Value o = json::Value::object();
    o.set("ok", json::Value::boolean(false));
    o.set("error", json::Value::string("the session settings are not a JSON object"));
    return dup_json(o);
  }
  return dup_json(
      browserstack::start_session(*c, browserstack::AutomateSpec::from_json(*spec)).to_json());
}

char* mpi_bs_automate_screenshot_json(const char* session_id, const char* out_path) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  json::Value o = json::Value::object();
  const auto s = browserstack::screenshot(*c, safe(session_id));
  if (s.ok) {
    // Written beside and renamed over, so a reader never sees half a PNG.
    const std::string out = safe(out_path);
    const std::string tmp = out + ".part";
    {
      std::ofstream f(tmp, std::ios::binary);
      f.write(s.png.data(), static_cast<std::streamsize>(s.png.size()));
    }
    if (std::rename(tmp.c_str(), out.c_str()) != 0) {
      o.set("ok", json::Value::boolean(false));
      o.set("error", json::Value::string("could not write " + out));
      return dup_json(o);
    }
    o.set("path", json::Value::string(out));
    o.set("width", json::Value::integer(s.width));
    o.set("height", json::Value::integer(s.height));
  } else {
    o.set("error", json::Value::string(s.error));
  }
  o.set("ok", json::Value::boolean(s.ok));
  return dup_json(o);
}

char* mpi_bs_automate_input_json(const char* session_id, const char* action_json) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  const std::string sid = safe(session_id);
  const auto a = json::parse(safe(action_json), nullptr);
  auto num = [&](const json::Value& arr, std::size_t i) {
    return i < arr.items().size() && arr.items()[i].is_number()
               ? static_cast<int>(arr.items()[i].as_double())
               : 0;
  };
  browserstack::Reply r;
  if (a && a->find("tap") != nullptr && a->find("tap")->is_array()) {
    const auto& p = *a->find("tap");
    r = browserstack::tap(*c, sid, num(p, 0), num(p, 1));
  } else if (a && a->find("swipe") != nullptr && a->find("swipe")->is_array()) {
    const auto& p = *a->find("swipe");
    r = browserstack::swipe(*c, sid, num(p, 0), num(p, 1), num(p, 2), num(p, 3));
  } else if (a && a->find("text") != nullptr && a->find("text")->is_string()) {
    r = browserstack::type_text(*c, sid, a->find("text")->as_string());
  } else if (a && a->find("key") != nullptr && a->find("key")->is_string()) {
    r = browserstack::press(*c, sid, a->find("key")->as_string());
  } else {
    r.error = "an action is {\"tap\": [x, y]}, {\"swipe\": [x1, y1, x2, y2]}, "
              "{\"text\": \"...\"} or {\"key\": \"back|home|enter\"}";
  }
  return dup_json(r.to_json());
}

char* mpi_bs_automate_stop_json(const char* session_id, const char* import_to_sessions_dir) {
  const auto c = browserstack::find_credentials();
  if (!c) return dup_json(no_credentials());
  return dup_json(browserstack::stop_session(*c, safe(session_id),
                                             safe(import_to_sessions_dir),
                                             capi::current_cancel_token())
                      .to_json());
}

}  // extern "C"
