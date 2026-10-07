// A BrowserStack App Automate session that DevX starts and drives itself: a
// real device in BrowserStack's cloud, with BrowserStack's own settings
// (network profile, GPS, timezone, locale, orientation, biometrics, camera
// injection, App Profiling), its screen shown in DevX, and input sent to it.
//
// The session is a WebDriver (Appium) session on hub-cloud.browserstack.com.
// It lives on BrowserStack's side, so any process holding the credentials
// can drive it by its id: the window, `mpi browserstack`, and `mpi mcp`
// alike. Minutes are BrowserStack's to bill while it runs, and BrowserStack
// ends a session left idle (DevX asks for 300 s).
//
// On stop, a session started with App Profiling is imported as a DevX session
// (profiling.hpp), once BrowserStack has published it.
#pragma once

#include <string>

#include "adapters/browserstack/browserstack.hpp"
#include "adapters/browserstack/profiling.hpp"
#include "core/util/json.hpp"

namespace mpi::browserstack {

struct AutomateSpec {
  std::string app_url;      // bs://... from an App Automate upload
  std::string platform;     // android | ios
  std::string device;       // as BrowserStack names it, e.g. "Google Pixel 8"
  std::string os_version;   // e.g. "14.0"
  std::string project = "DevX";
  std::string build = "DevX";
  std::string name;
  // BrowserStack's settings; an empty one is not sent.
  std::string network_profile;  // e.g. "4g-lte-good", "3g-umts-good", "no-network"
  std::string gps_location;     // "lat,lng"
  std::string timezone;         // e.g. "Tokyo"
  std::string language;         // e.g. "fr"
  std::string locale;           // e.g. "FR"
  std::string orientation;      // portrait | landscape
  bool biometric = false;
  bool camera_injection = false;
  bool app_profiling = false;
  bool local = false;           // through BrowserStack Local (bs-local.com)
  std::string local_identifier;

  /// The W3C new-session body.
  json::Value capabilities() const;
  /// From the same field names as this struct ("app_url", "device",
  /// "network_profile", "app_profiling", ...), as the window and `mpi mcp`
  /// send them.
  static AutomateSpec from_json(const json::Value& o);
};

struct AutomateStart {
  bool ok = false;
  std::string error;
  std::string session_id;
  json::Value reply;        // BrowserStack's, as it came
  json::Value to_json() const;
};

/// Starts the session: BrowserStack picks the device, installs the app and
/// launches it, which takes from a few seconds to a minute or two.
AutomateStart start_session(const Credentials& c, const AutomateSpec& spec);

/// The screen as PNG bytes, in the device's pixels.
struct Screenshot {
  bool ok = false;
  std::string error;
  std::string png;
  int width = 0, height = 0;  // the PNG's
};
Screenshot screenshot(const Credentials& c, const std::string& session_id);

/// Input in screenshot pixels; mapped to the session's coordinates (iOS
/// counts points, so a 3x screen's pixel is a third of a point).
Reply tap(const Credentials& c, const std::string& session_id, int x, int y);
Reply swipe(const Credentials& c, const std::string& session_id, int x1, int y1, int x2,
            int y2, int duration_ms = 300);
/// Types into whatever has focus.
Reply type_text(const Credentials& c, const std::string& session_id, const std::string& text);
/// back | home | enter
Reply press(const Credentials& c, const std::string& session_id, const std::string& key);

struct AutomateStop {
  bool ok = false;
  std::string error;
  json::Value details;          // the session as BrowserStack reports it afterwards
  bool imported = false;
  ProfilingImport profiling;    // when asked for
  json::Value to_json() const;
};

/// Ends the session, marks it with `status` (passed | failed | empty to leave
/// it), and with `import_to` set, imports its App Profiling there. BrowserStack
/// publishes profiling a little after the session ends, so the import waits
/// for it, up to `wait`.
AutomateStop stop_session(const Credentials& c, const std::string& session_id,
                          const std::string& import_to, const CancellationToken& cancel,
                          std::chrono::seconds wait = std::chrono::seconds(90));

/// The base64 standard alphabet, padding optional. Exposed for the tests.
bool base64_decode(const std::string& in, std::string* out);

/// The pixel size of a PNG, from its IHDR. Exposed for the tests.
bool png_size(const std::string& png, int* width, int* height);

}  // namespace mpi::browserstack
