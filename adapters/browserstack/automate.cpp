#include "adapters/browserstack/automate.hpp"

#include <map>
#include <mutex>
#include <thread>

namespace mpi::browserstack {
namespace {

constexpr const char* kHub = "https://hub-cloud.browserstack.com/wd/hub";

bool session_id_ok(const std::string& s) {
  return !s.empty() && s.size() < 128 &&
         s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") ==
             std::string::npos;
}

Reply refused(const std::string& why) {
  Reply r;
  r.error = why;
  return r;
}

// WebDriver puts a failure's reason in value.message; surface it as the error.
Reply hub(const Credentials& c, const std::string& method, const std::string& session_id,
          const std::string& path, const json::Value& body,
          std::chrono::seconds timeout = std::chrono::seconds(60)) {
  if (!session_id_ok(session_id)) return refused("not a session id: '" + session_id + "'");
  Reply r = request(c, method, std::string(kHub) + "/session/" + session_id + path,
                    body.is_null() ? std::string() : body.dump(), timeout);
  if (!r.ok && r.error.empty()) {
    const json::Value* v = r.body.find("value");
    const json::Value* m = v != nullptr ? v->find("message") : nullptr;
    r.error = m != nullptr && m->is_string()
                  ? m->as_string()
                  : "BrowserStack answered HTTP " + std::to_string(r.http_status);
  }
  return r;
}

void set_if(json::Value& o, const char* key, const std::string& v) {
  if (!v.empty()) o.set(key, json::Value::string(v));
}

// Per session: its platform (for Home) and how screenshot pixels map to its
// coordinates. Both are learnt once and kept for the life of the process.
struct SessionInfo {
  std::string platform;
  int png_width = 0;
  double scale = 0;  // session units per screenshot pixel
};
std::mutex g_mu;
std::map<std::string, SessionInfo> g_sessions;

double scale_for(const Credentials& c, const std::string& sid) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_sessions.find(sid);
    if (it != g_sessions.end() && it->second.scale > 0) return it->second.scale;
  }
  int png_width = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    png_width = g_sessions[sid].png_width;
  }
  if (png_width == 0) png_width = screenshot(c, sid).width;
  const Reply rect = hub(c, "GET", sid, "/window/rect", json::Value());
  const json::Value* v = rect.body.find("value");
  const json::Value* w = v != nullptr ? v->find("width") : nullptr;
  double scale = 1;
  if (w != nullptr && w->is_number() && png_width > 0) {
    scale = w->as_double() / png_width;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  g_sessions[sid].scale = scale;
  return scale;
}

json::Value pointer(std::initializer_list<json::Value> steps) {
  json::Value params = json::Value::object();
  params.set("pointerType", json::Value::string("touch"));
  json::Value seq = json::Value::array();
  for (const auto& s : steps) seq.push_back(s);
  json::Value src = json::Value::object();
  src.set("type", json::Value::string("pointer"));
  src.set("id", json::Value::string("finger1"));
  src.set("parameters", params);
  src.set("actions", seq);
  json::Value all = json::Value::array();
  all.push_back(src);
  json::Value body = json::Value::object();
  body.set("actions", all);
  return body;
}

json::Value step(const char* type, int x = -1, int y = -1, int duration = -1) {
  json::Value s = json::Value::object();
  s.set("type", json::Value::string(type));
  if (x >= 0) {
    s.set("x", json::Value::integer(x));
    s.set("y", json::Value::integer(y));
  }
  if (duration >= 0) s.set("duration", json::Value::integer(duration));
  if (std::string(type) == "pointerDown" || std::string(type) == "pointerUp") {
    s.set("button", json::Value::integer(0));
  }
  return s;
}

json::Value execute(const std::string& script, json::Value arg) {
  json::Value args = json::Value::array();
  args.push_back(std::move(arg));
  json::Value body = json::Value::object();
  body.set("script", json::Value::string(script));
  body.set("args", args);
  return body;
}

// Key actions for each UTF-8 code point of `text`.
json::Value keys(const std::string& text) {
  json::Value seq = json::Value::array();
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char b = static_cast<unsigned char>(text[i]);
    const std::size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    const std::string ch = text.substr(i, n);
    i += n;
    for (const char* t : {"keyDown", "keyUp"}) {
      json::Value k = json::Value::object();
      k.set("type", json::Value::string(t));
      k.set("value", json::Value::string(ch));
      seq.push_back(k);
    }
  }
  json::Value src = json::Value::object();
  src.set("type", json::Value::string("key"));
  src.set("id", json::Value::string("keyboard"));
  src.set("actions", seq);
  json::Value all = json::Value::array();
  all.push_back(src);
  json::Value body = json::Value::object();
  body.set("actions", all);
  return body;
}

}  // namespace

json::Value AutomateSpec::capabilities() const {
  json::Value bs = json::Value::object();
  set_if(bs, "deviceName", device);
  set_if(bs, "osVersion", os_version);
  set_if(bs, "projectName", project);
  set_if(bs, "buildName", build);
  set_if(bs, "sessionName", name);
  bs.set("idleTimeout", json::Value::integer(300));
  set_if(bs, "networkProfile", network_profile);
  set_if(bs, "gpsLocation", gps_location);
  set_if(bs, "timezone", timezone);
  set_if(bs, "deviceOrientation", orientation);
  if (biometric) bs.set("enableBiometric", json::Value::boolean(true));
  if (camera_injection) bs.set("enableCameraImageInjection", json::Value::boolean(true));
  if (app_profiling) bs.set("appProfiling", json::Value::boolean(true));
  if (local) {
    bs.set("local", json::Value::boolean(true));
    set_if(bs, "localIdentifier", local_identifier);
  }

  json::Value always = json::Value::object();
  always.set("platformName", json::Value::string(platform));
  always.set("appium:app", json::Value::string(app_url));
  set_if(always, "appium:language", language);
  set_if(always, "appium:locale", locale);
  always.set("bstack:options", bs);
  json::Value caps = json::Value::object();
  caps.set("alwaysMatch", always);
  json::Value body = json::Value::object();
  body.set("capabilities", caps);
  return body;
}

AutomateSpec AutomateSpec::from_json(const json::Value& o) {
  auto s = [&](const char* k, const std::string& fallback = {}) {
    const json::Value* v = o.find(k);
    return v != nullptr && v->is_string() ? v->as_string() : fallback;
  };
  auto b = [&](const char* k) {
    const json::Value* v = o.find(k);
    return v != nullptr && v->as_bool(false);
  };
  AutomateSpec spec;
  spec.app_url = s("app_url");
  spec.platform = s("platform", "android");
  spec.device = s("device");
  spec.os_version = s("os_version");
  spec.project = s("project", spec.project);
  spec.build = s("build", spec.build);
  spec.name = s("name");
  spec.network_profile = s("network_profile");
  spec.gps_location = s("gps_location");
  spec.timezone = s("timezone");
  spec.language = s("language");
  spec.locale = s("locale");
  spec.orientation = s("orientation");
  spec.biometric = b("biometric");
  spec.camera_injection = b("camera_injection");
  spec.app_profiling = b("app_profiling");
  spec.local = b("local");
  spec.local_identifier = s("local_identifier");
  return spec;
}

json::Value AutomateStart::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!session_id.empty()) o.set("session_id", json::Value::string(session_id));
  if (!reply.is_null()) o.set("reply", reply);
  return o;
}

AutomateStart start_session(const Credentials& c, const AutomateSpec& spec) {
  AutomateStart out;
  if (spec.platform != "android" && spec.platform != "ios") {
    out.error = "platform is android or ios";
    return out;
  }
  if (spec.app_url.rfind("bs://", 0) != 0) {
    out.error = "the app is a bs:// URL from an App Automate upload";
    return out;
  }
  if (spec.device.empty() || spec.os_version.empty()) {
    out.error = "a device and its OS version are needed";
    return out;
  }
  // BrowserStack queues, installs and launches before it answers.
  const Reply r = request(c, "POST", std::string(kHub) + "/session", spec.capabilities().dump(),
                          std::chrono::seconds(600));
  out.reply = r.body;
  const json::Value* v = r.body.find("value");
  const json::Value* sid = v != nullptr ? v->find("sessionId") : nullptr;
  if (r.ok && sid != nullptr && sid->is_string()) {
    out.ok = true;
    out.session_id = sid->as_string();
    std::lock_guard<std::mutex> lock(g_mu);
    g_sessions[out.session_id].platform = spec.platform;
    return out;
  }
  const json::Value* m = v != nullptr ? v->find("message") : nullptr;
  out.error = !r.error.empty() ? r.error
              : m != nullptr && m->is_string()
                  ? m->as_string()
                  : "BrowserStack answered HTTP " + std::to_string(r.http_status);
  return out;
}

Screenshot screenshot(const Credentials& c, const std::string& session_id) {
  Screenshot s;
  const Reply r = hub(c, "GET", session_id, "/screenshot", json::Value());
  const json::Value* v = r.body.find("value");
  if (!r.ok || v == nullptr || !v->is_string()) {
    s.error = r.error.empty() ? "no screenshot in the reply" : r.error;
    return s;
  }
  if (!base64_decode(v->as_string(), &s.png) || !png_size(s.png, &s.width, &s.height)) {
    s.error = "the screenshot is not a PNG";
    s.png.clear();
    return s;
  }
  s.ok = true;
  std::lock_guard<std::mutex> lock(g_mu);
  g_sessions[session_id].png_width = s.width;
  return s;
}

Reply tap(const Credentials& c, const std::string& session_id, int x, int y) {
  const double k = scale_for(c, session_id);
  const int sx = static_cast<int>(x * k), sy = static_cast<int>(y * k);
  return hub(c, "POST", session_id, "/actions",
             pointer({step("pointerMove", sx, sy, 0), step("pointerDown"),
                      step("pause", -1, -1, 80), step("pointerUp")}));
}

Reply swipe(const Credentials& c, const std::string& session_id, int x1, int y1, int x2, int y2,
            int duration_ms) {
  const double k = scale_for(c, session_id);
  return hub(c, "POST", session_id, "/actions",
             pointer({step("pointerMove", static_cast<int>(x1 * k), static_cast<int>(y1 * k), 0),
                      step("pointerDown"), step("pause", -1, -1, 50),
                      step("pointerMove", static_cast<int>(x2 * k), static_cast<int>(y2 * k),
                           duration_ms),
                      step("pointerUp")}));
}

Reply type_text(const Credentials& c, const std::string& session_id, const std::string& text) {
  if (text.empty()) return refused("nothing to type");
  return hub(c, "POST", session_id, "/actions", keys(text));
}

Reply press(const Credentials& c, const std::string& session_id, const std::string& key) {
  if (key == "back") return hub(c, "POST", session_id, "/back", json::Value::object());
  if (key == "enter") return hub(c, "POST", session_id, "/actions", keys("\xEE\x80\x87"));  // U+E007
  if (key != "home") return refused("key is back, home or enter");
  std::string platform;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    platform = g_sessions[session_id].platform;
  }
  if (platform.empty()) {
    // A session another process started: BrowserStack knows its OS.
    const Reply d = get(c, "app-automate/sessions/" + session_id + ".json");
    const json::Value* s = d.body.find("automation_session");
    const json::Value* os = s != nullptr ? s->find("os") : nullptr;
    platform = os != nullptr && os->is_string() ? os->as_string() : "android";
    std::lock_guard<std::mutex> lock(g_mu);
    g_sessions[session_id].platform = platform;
  }
  if (platform == "ios") {
    json::Value ios = json::Value::object();
    ios.set("name", json::Value::string("home"));
    return hub(c, "POST", session_id, "/execute/sync", execute("mobile: pressButton", ios));
  }
  // BrowserStack's UiAutomator2 offers no `mobile: pressKey`; Appium's own
  // key endpoint is there.
  json::Value android = json::Value::object();
  android.set("keycode", json::Value::integer(3));
  return hub(c, "POST", session_id, "/appium/device/press_keycode", android);
}

json::Value AutomateStop::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!details.is_null()) o.set("session", details);
  if (imported || !profiling.error.empty()) o.set("profiling", profiling.to_json());
  return o;
}

AutomateStop stop_session(const Credentials& c, const std::string& session_id,
                          const std::string& import_to, const CancellationToken& cancel,
                          std::chrono::seconds wait) {
  AutomateStop out;
  const Reply r = hub(c, "DELETE", session_id, "", json::Value());
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_sessions.erase(session_id);
  }
  // A session BrowserStack already ended (idle timeout) answers 404 here; it
  // is stopped all the same.
  if (!r.ok && r.http_status != 404) {
    out.error = r.error;
    return out;
  }
  out.ok = true;
  const Reply d = get(c, "app-automate/sessions/" + session_id + ".json");
  if (d.ok) {
    const json::Value* s = d.body.find("automation_session");
    out.details = s != nullptr ? *s : d.body;
  }
  if (import_to.empty()) return out;

  ProfilingRequest req;
  req.session_id = session_id;
  req.sessions_dir = import_to;
  const auto deadline = std::chrono::steady_clock::now() + wait;
  for (;;) {
    out.profiling = import_profiling(c, req, cancel);
    if (out.profiling.ok || cancel.cancelled() ||
        std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    for (int i = 0; i < 100 && !cancel.cancelled(); i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  out.imported = out.profiling.ok;
  return out;
}

bool base64_decode(const std::string& in, std::string* out) {
  out->clear();
  out->reserve(in.size() * 3 / 4);
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char ch : in) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else if (ch == '=') break;
    else if (ch == '\n' || ch == '\r') continue;
    else return false;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

bool png_size(const std::string& png, int* width, int* height) {
  static const char kSig[] = "\x89PNG\r\n\x1a\n";
  if (png.size() < 24 || png.compare(0, 8, kSig, 8) != 0 || png.compare(12, 4, "IHDR") != 0) {
    return false;
  }
  auto be32 = [&](std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 3]));
  };
  *width = static_cast<int>(be32(16));
  *height = static_cast<int>(be32(20));
  return *width > 0 && *height > 0;
}

}  // namespace mpi::browserstack
