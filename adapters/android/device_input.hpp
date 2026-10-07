// Driving an Android device's screen through adb: tap, swipe, text, keys.
//
// `adb shell input` works on every Android device and emulator, release build
// or debug, so this is what an AI tool uses to operate an app it is testing.
// Coordinates are device pixels, the same space as a full-size screenshot and
// as the frames in a layout snapshot -- so a model can find an element with
// capture_layout and tap the middle of it.
#pragma once

#include <string>

namespace mpi::android {

bool input_tap(const std::string& adb, const std::string& serial, int x, int y,
               std::string* error);
bool input_swipe(const std::string& adb, const std::string& serial, int x1, int y1, int x2,
                 int y2, int duration_ms, std::string* error);
/// Types `text` into whatever has focus. Spaces and shell-special characters
/// are escaped for `input text`; characters outside printable ASCII are
/// refused, because `input text` drops them silently.
bool input_text(const std::string& adb, const std::string& serial, const std::string& text,
                std::string* error);
/// back | home | recents | enter | delete | tab | escape | power | volume_up |
/// volume_down | menu, or a raw KEYCODE_* name.
bool input_key(const std::string& adb, const std::string& serial, const std::string& key,
               std::string* error);

/// The adb a call should use: the SDK's platform-tools when there is one,
/// otherwise `adb` on PATH.
std::string adb_for_input();

}  // namespace mpi::android
