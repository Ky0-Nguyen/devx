// Android Virtual Devices, without avdmanager.
//
// An AVD is two plain files -- `<id>.ini` naming its directory and
// `<id>.avd/config.ini` holding its hardware -- under ~/.android/avd (or
// $ANDROID_AVD_HOME). Android Studio and the emulator read the same files, so
// an AVD made here shows up in Android Studio's Device Manager and the other
// way round.
//
// Screen sizes come from presets: the reference devices Android's window
// size classes are defined by, and real Pixel devices whose pixel sizes are
// the SDK's own device skins. Every preset states its size in dp and its
// window size class, and any of them can be changed.
#pragma once

#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::android {

struct DevicePreset {
  std::string id;
  std::string name;
  std::string category;  // phone | foldable | tablet | desktop
  int width = 0;         // pixels, in the device's natural orientation
  int height = 0;
  int density = 0;       // dpi, hw.lcd.density
  double diagonal_in = 0;
  std::string basis;     // where the numbers come from
};

const std::vector<DevicePreset>& device_presets();
const DevicePreset* find_preset(const std::string& id);

/// Android's window size class for a width in pixels at a density:
/// compact < 600 dp <= medium < 840 dp <= expanded.
std::string window_size_class(int width_px, int density);
int px_to_dp(int px, int density);
json::Value preset_json(const DevicePreset& p);

std::string avd_home();

struct AvdInfo {
  std::string id;            // the file name, and what -avd takes
  std::string display_name;
  std::string directory;
  int width = 0, height = 0, density = 0;
  int ram_mb = 0;
  std::string system_image;  // the package path its image.sysdir names
  std::string image_dir;     // relative to the SDK root, as config.ini has it
  int api_level = 0;
  std::string tag;
  std::string abi;
  bool play_store = false;
  json::Value to_json() const;
};

std::vector<AvdInfo> list_avds();
bool read_avd(const std::string& id, AvdInfo& out, std::string* error);

struct AvdSpec {
  std::string display_name;
  std::string system_image;  // "system-images;android-35;google_apis_playstore;arm64-v8a"
  int width = 0, height = 0, density = 0;
  int ram_mb = 2048;
  int storage_mb = 6144;
  std::string device_name;   // a preset id, recorded as hw.device.name
};

/// Writes a new AVD. The id is derived from the name (letters, digits and
/// underscores) and must not exist yet.
bool create_avd(const std::string& sdk_root, const AvdSpec& spec, AvdInfo& out,
                std::string* error);
/// Removes an AVD's files. The caller checks it is not running first.
bool delete_avd(const std::string& id, std::string* error);
/// A file in an AVD's directory asking for the next boot to be cold.
extern const char* const kColdBootMarker;

/// Changes the hardware screen. Takes effect at the next boot, which is made a
/// cold one because a snapshot holds the old screen.
bool set_avd_screen(const std::string& id, int width, int height, int density,
                    std::string* error);

/// An id from a display name: what Android Studio does, spaces to underscores.
std::string avd_id_for(const std::string& display_name);

}  // namespace mpi::android
