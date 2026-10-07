#include "adapters/android/device_input.hpp"

#include <unistd.h>

#include <map>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/emulator/sdk_repository.hpp"
#include "core/util/process.hpp"

namespace mpi::android {
namespace {

bool shell_input(const std::string& adb, const std::string& serial,
                 const std::vector<std::string>& args, std::string* error) {
  if (!proc::is_safe_argument(serial, true)) {
    if (error != nullptr) *error = "refusing a serial that would be read as an option";
    return false;
  }
  std::vector<std::string> argv = {adb, "-s", serial, "shell", "input"};
  argv.insert(argv.end(), args.begin(), args.end());
  proc::Options po;
  po.timeout = std::chrono::seconds(15);
  const auto r = proc::run(argv, po);
  if (!r.ok()) {
    if (error != nullptr) {
      *error = r.spawned ? (r.err.empty() ? r.out : r.err) : r.spawn_error;
      if (error->empty()) *error = "adb input failed";
    }
    return false;
  }
  return true;
}

}  // namespace

std::string adb_for_input() {
  const std::string sdk_adb = locate_sdk().root + "/platform-tools/adb";
  if (::access(sdk_adb.c_str(), X_OK) == 0) return sdk_adb;
  return default_adb_path();
}

bool input_tap(const std::string& adb, const std::string& serial, int x, int y,
               std::string* error) {
  return shell_input(adb, serial, {"tap", std::to_string(x), std::to_string(y)}, error);
}

bool input_swipe(const std::string& adb, const std::string& serial, int x1, int y1, int x2,
                 int y2, int duration_ms, std::string* error) {
  return shell_input(adb, serial,
                     {"swipe", std::to_string(x1), std::to_string(y1), std::to_string(x2),
                      std::to_string(y2), std::to_string(duration_ms > 0 ? duration_ms : 300)},
                     error);
}

bool input_text(const std::string& adb, const std::string& serial, const std::string& text,
                std::string* error) {
  // `input text` runs through the device's shell, so every shell-special
  // character is escaped, and a space must be %s.
  std::string escaped;
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7e) {
      if (error != nullptr) {
        *error = "only printable ASCII can be typed through `adb shell input text`; "
                 "it drops anything else without saying so";
      }
      return false;
    }
    if (c == ' ') {
      escaped += "%s";
    } else if (std::string("\\\"'`$&|;<>()[]{}*?!~#%^").find(c) != std::string::npos) {
      escaped.push_back('\\');
      escaped.push_back(c);
    } else {
      escaped.push_back(c);
    }
  }
  if (escaped.empty()) return true;
  return shell_input(adb, serial, {"text", escaped}, error);
}

bool input_key(const std::string& adb, const std::string& serial, const std::string& key,
               std::string* error) {
  static const std::map<std::string, std::string> names = {
      {"back", "KEYCODE_BACK"},         {"home", "KEYCODE_HOME"},
      {"recents", "KEYCODE_APP_SWITCH"}, {"enter", "KEYCODE_ENTER"},
      {"delete", "KEYCODE_DEL"},         {"tab", "KEYCODE_TAB"},
      {"escape", "KEYCODE_ESCAPE"},      {"power", "KEYCODE_POWER"},
      {"volume_up", "KEYCODE_VOLUME_UP"}, {"volume_down", "KEYCODE_VOLUME_DOWN"},
      {"menu", "KEYCODE_MENU"},
  };
  std::string code;
  const auto it = names.find(key);
  if (it != names.end()) {
    code = it->second;
  } else if (key.rfind("KEYCODE_", 0) == 0 &&
             key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
    code = key;
  } else {
    if (error != nullptr) {
      *error = "unknown key '" + key + "'; use back, home, recents, enter, delete, tab, "
               "escape, power, volume_up, volume_down, menu, or a KEYCODE_* name";
    }
    return false;
  }
  return shell_input(adb, serial, {"keyevent", code}, error);
}

}  // namespace mpi::android
