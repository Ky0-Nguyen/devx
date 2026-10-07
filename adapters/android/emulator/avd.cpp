#include "adapters/android/emulator/avd.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace mpi::android {
namespace {

namespace fs = std::filesystem;

std::map<std::string, std::string> read_ini(const std::string& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    out[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return out;
}

bool write_ini(const std::string& path, const std::map<std::string, std::string>& kv,
               std::string* error) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    for (const auto& [k, v] : kv) out << k << "=" << v << "\n";
    if (!out) {
      if (error != nullptr) *error = "could not write " + path;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec && error != nullptr) *error = "could not write " + path + ": " + ec.message();
  return !ec;
}

int to_int(const std::map<std::string, std::string>& kv, const char* key) {
  const auto it = kv.find(key);
  return it == kv.end() ? 0 : std::atoi(it->second.c_str());
}

std::string get(const std::map<std::string, std::string>& kv, const char* key) {
  const auto it = kv.find(key);
  return it == kv.end() ? std::string() : it->second;
}

// "6G", "2048M", "2048" -> megabytes.
int megabytes(const std::string& v) {
  if (v.empty()) return 0;
  const int n = std::atoi(v.c_str());
  const char unit = v.back();
  if (unit == 'G' || unit == 'g') return n * 1024;
  return n;
}

}  // namespace

const char* const kColdBootMarker = ".devx-cold-boot-next";

const std::vector<DevicePreset>& device_presets() {
  static const std::vector<DevicePreset> presets = {
      // Reference devices for the window size classes (developer.android.com,
      // "Window size classes"), at the densities Android Studio's generic
      // devices use.
      {"small_phone", "Small Phone", "phone", 720, 1280, 320, 4.65,
       "generic: 360 x 640 dp, the smallest common phone"},
      {"medium_phone", "Medium Phone", "phone", 1080, 2400, 420, 6.4,
       "generic: 411 x 914 dp, the reference compact-width phone"},
      {"medium_foldable", "Foldable (unfolded)", "foldable", 1766, 2208, 420, 7.6,
       "generic: 673 x 841 dp, the reference medium-width foldable"},
      {"medium_tablet", "Medium Tablet", "tablet", 2560, 1600, 320, 10.1,
       "generic: 1280 x 800 dp, the reference expanded-width tablet"},
      {"desktop", "Desktop", "desktop", 1920, 1080, 160, 15.6,
       "generic: 1920 x 1080 dp, the reference desktop window"},
      // Real devices: pixel sizes from the SDK's device skins.
      {"pixel_9", "Pixel 9", "phone", 1080, 2424, 420, 6.3, "SDK skin pixel_9"},
      {"pixel_9_pro", "Pixel 9 Pro", "phone", 1280, 2856, 480, 6.3, "SDK skin pixel_9_pro"},
      {"pixel_9_pro_xl", "Pixel 9 Pro XL", "phone", 1344, 2992, 480, 6.8,
       "SDK skin pixel_9_pro_xl"},
      {"pixel_8a", "Pixel 8a", "phone", 1080, 2400, 420, 6.1, "SDK skin pixel_8a"},
      {"pixel_6a", "Pixel 6a", "phone", 1080, 2400, 420, 6.1, "SDK skin pixel_6a"},
      {"pixel_tablet", "Pixel Tablet", "tablet", 2560, 1600, 320, 10.95,
       "SDK skin pixel_tablet"},
      {"tablet_7in", "7\" Tablet", "tablet", 1200, 1920, 320, 7.0, "SDK skin nexus_7_2013"},
  };
  return presets;
}

const DevicePreset* find_preset(const std::string& id) {
  for (const auto& p : device_presets()) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

int px_to_dp(int px, int density) {
  return density > 0 ? static_cast<int>(std::lround(px * 160.0 / density)) : 0;
}

std::string window_size_class(int width_px, int density) {
  const int dp = px_to_dp(width_px, density);
  if (dp < 600) return "compact";
  if (dp < 840) return "medium";
  return "expanded";
}

json::Value preset_json(const DevicePreset& p) {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(p.id));
  o.set("name", json::Value::string(p.name));
  o.set("category", json::Value::string(p.category));
  o.set("width", json::Value::integer(p.width));
  o.set("height", json::Value::integer(p.height));
  o.set("density", json::Value::integer(p.density));
  o.set("width_dp", json::Value::integer(px_to_dp(p.width, p.density)));
  o.set("height_dp", json::Value::integer(px_to_dp(p.height, p.density)));
  o.set("diagonal_in", json::Value::number(p.diagonal_in));
  // Both orientations: a phone is compact upright and may be medium on its
  // side. The width and height are the device's natural orientation, which is
  // landscape for a tablet or a desktop.
  o.set("window_class", json::Value::string(window_size_class(p.width, p.density)));
  o.set("window_class_rotated", json::Value::string(window_size_class(p.height, p.density)));
  o.set("basis", json::Value::string(p.basis));
  return o;
}

std::string avd_home() {
  if (const char* v = std::getenv("ANDROID_AVD_HOME"); v != nullptr && *v != '\0') return v;
  const char* h = std::getenv("HOME");
  return std::string(h != nullptr ? h : "") + "/.android/avd";
}

std::string avd_id_for(const std::string& display_name) {
  std::string id;
  for (char c : display_name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.') {
      id.push_back(c);
    } else if (c == ' ') {
      id.push_back('_');
    }
  }
  return id;
}

json::Value AvdInfo::to_json() const {
  json::Value o = json::Value::object();
  o.set("id", json::Value::string(id));
  o.set("display_name", json::Value::string(display_name));
  o.set("directory", json::Value::string(directory));
  o.set("width", json::Value::integer(width));
  o.set("height", json::Value::integer(height));
  o.set("density", json::Value::integer(density));
  o.set("width_dp", json::Value::integer(px_to_dp(width, density)));
  o.set("height_dp", json::Value::integer(px_to_dp(height, density)));
  o.set("window_class", json::Value::string(window_size_class(width, density)));
  o.set("ram_mb", json::Value::integer(ram_mb));
  o.set("system_image", json::Value::string(system_image));
  o.set("api_level", json::Value::integer(api_level));
  o.set("tag", json::Value::string(tag));
  o.set("abi", json::Value::string(abi));
  o.set("play_store", json::Value::boolean(play_store));
  return o;
}

bool read_avd(const std::string& id, AvdInfo& out, std::string* error) {
  const auto top = read_ini(avd_home() + "/" + id + ".ini");
  std::string dir = get(top, "path");
  if (dir.empty()) dir = avd_home() + "/" + id + ".avd";
  struct stat st {};
  if (::stat((dir + "/config.ini").c_str(), &st) != 0) {
    if (error != nullptr) *error = "no AVD '" + id + "'";
    return false;
  }
  const auto c = read_ini(dir + "/config.ini");
  out = AvdInfo{};
  out.id = id;
  out.directory = dir;
  out.display_name = get(c, "avd.ini.displayname");
  if (out.display_name.empty()) out.display_name = id;
  out.width = to_int(c, "hw.lcd.width");
  out.height = to_int(c, "hw.lcd.height");
  out.density = to_int(c, "hw.lcd.density");
  out.ram_mb = megabytes(get(c, "hw.ramSize"));
  out.image_dir = get(c, "image.sysdir.1");
  out.tag = get(c, "tag.id");
  out.abi = get(c, "abi.type");
  out.play_store = get(c, "PlayStore.enabled") == "true";
  // system-images/android-35/google_apis_playstore/arm64-v8a/ -> its package path.
  std::string rel = out.image_dir;
  while (!rel.empty() && rel.back() == '/') rel.pop_back();
  std::replace(rel.begin(), rel.end(), '/', ';');
  out.system_image = rel;
  const auto api = rel.find(";android-");
  if (api != std::string::npos) out.api_level = std::atoi(rel.c_str() + api + 9);
  return true;
}

std::vector<AvdInfo> list_avds() {
  std::vector<AvdInfo> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(avd_home(), ec)) {
    if (e.path().extension() != ".ini") continue;
    AvdInfo a;
    if (read_avd(e.path().stem().string(), a, nullptr)) out.push_back(std::move(a));
  }
  std::sort(out.begin(), out.end(),
            [](const AvdInfo& a, const AvdInfo& b) { return a.display_name < b.display_name; });
  return out;
}

bool create_avd(const std::string& sdk_root, const AvdSpec& spec, AvdInfo& out,
                std::string* error) {
  const std::string id = avd_id_for(spec.display_name);
  if (id.empty()) {
    if (error != nullptr) *error = "the name has no letters or digits to make an id from";
    return false;
  }
  if (spec.width <= 0 || spec.height <= 0 || spec.density <= 0) {
    if (error != nullptr) *error = "width, height and density must be positive";
    return false;
  }
  const std::string home = avd_home();
  const std::string dir = home + "/" + id + ".avd";
  std::error_code ec;
  if (fs::exists(home + "/" + id + ".ini", ec) || fs::exists(dir, ec)) {
    if (error != nullptr) *error = "an AVD with the id '" + id + "' already exists";
    return false;
  }
  // system-images;android-35;google_apis_playstore;arm64-v8a
  std::vector<std::string> parts;
  {
    std::stringstream ss(spec.system_image);
    std::string p;
    while (std::getline(ss, p, ';')) parts.push_back(p);
  }
  if (parts.size() != 4 || parts[0] != "system-images") {
    if (error != nullptr) *error = "not a system image package path: " + spec.system_image;
    return false;
  }
  const std::string image_rel = "system-images/" + parts[1] + "/" + parts[2] + "/" + parts[3] + "/";
  if (!fs::exists(sdk_root + "/" + image_rel + "system.img", ec) &&
      !fs::exists(sdk_root + "/" + image_rel + "package.xml", ec)) {
    if (error != nullptr) *error = spec.system_image + " is not installed in " + sdk_root;
    return false;
  }
  const bool play = parts[2].find("playstore") != std::string::npos;
  std::string tag_display = parts[2] == "google_apis_playstore" ? "Google Play"
                            : parts[2] == "google_apis"         ? "Google APIs"
                                                                : parts[2];
  fs::create_directories(dir, ec);
  if (ec) {
    if (error != nullptr) *error = "could not create " + dir + ": " + ec.message();
    return false;
  }
  std::map<std::string, std::string> config = {
      {"AvdId", id},
      {"avd.ini.displayname", spec.display_name},
      {"avd.ini.encoding", "UTF-8"},
      {"abi.type", parts[3]},
      {"hw.cpu.arch", parts[3] == "arm64-v8a" ? "arm64" : "x86_64"},
      {"image.sysdir.1", image_rel},
      {"tag.id", parts[2]},
      {"tag.display", tag_display},
      {"PlayStore.enabled", play ? "true" : "false"},
      {"hw.lcd.width", std::to_string(spec.width)},
      {"hw.lcd.height", std::to_string(spec.height)},
      {"hw.lcd.density", std::to_string(spec.density)},
      {"hw.ramSize", std::to_string(spec.ram_mb)},
      {"disk.dataPartition.size", std::to_string(spec.storage_mb) + "M"},
      {"hw.keyboard", "yes"},
      {"hw.gpu.enabled", "yes"},
      {"hw.gpu.mode", "auto"},
      {"hw.mainKeys", "no"},
      {"hw.sdCard", "yes"},
      {"sdcard.size", "512M"},
      {"showDeviceFrame", "no"},
      {"skin.dynamic", "yes"},
      {"fastboot.forceColdBoot", "no"},
      {"fastboot.forceFastBoot", "yes"},
      {"runtime.network.latency", "none"},
      {"runtime.network.speed", "full"},
  };
  if (!spec.device_name.empty()) config["hw.device.name"] = spec.device_name;
  std::map<std::string, std::string> top = {
      {"avd.ini.encoding", "UTF-8"},
      {"path", dir},
      {"path.rel", "avd/" + id + ".avd"},
      {"target", parts[1]},
  };
  if (!write_ini(dir + "/config.ini", config, error) ||
      !write_ini(home + "/" + id + ".ini", top, error)) {
    fs::remove_all(dir, ec);
    fs::remove(home + "/" + id + ".ini", ec);
    return false;
  }
  return read_avd(id, out, error);
}

bool delete_avd(const std::string& id, std::string* error) {
  AvdInfo a;
  if (!read_avd(id, a, error)) return false;
  std::error_code ec;
  fs::remove_all(a.directory, ec);
  fs::remove(avd_home() + "/" + id + ".ini", ec);
  if (ec && error != nullptr) *error = ec.message();
  return !ec;
}

bool set_avd_screen(const std::string& id, int width, int height, int density,
                    std::string* error) {
  AvdInfo a;
  if (!read_avd(id, a, error)) return false;
  if (width <= 0 || height <= 0 || density <= 0) {
    if (error != nullptr) *error = "width, height and density must be positive";
    return false;
  }
  auto c = read_ini(a.directory + "/config.ini");
  c["hw.lcd.width"] = std::to_string(width);
  c["hw.lcd.height"] = std::to_string(height);
  c["hw.lcd.density"] = std::to_string(density);
  if (!write_ini(a.directory + "/config.ini", c, error)) return false;
  // A snapshot holds the old screen, so the next boot must be cold -- once.
  // A marker rather than fastboot.forceColdBoot, which would stay set and
  // make every later boot cold too.
  std::ofstream(a.directory + "/" + kColdBootMarker) << "screen changed\n";
  return true;
}

}  // namespace mpi::android
