// Reading an Android app's view hierarchy through `dumpsys activity top`.
//
// Nothing is added to the app and nothing runs inside it: the platform prints
// the hierarchy of the top activity of every visible task, and
// observe::parse_android_dumpsys takes the one belonging to the package
// asked about. It works on an emulator and on a physical device, for a
// release build as well as a debug one.
#pragma once

#include <chrono>
#include <string>

#include "core/util/cancel.hpp"

namespace mpi::android {

struct ViewDump {
  bool ok = false;
  std::string text;
  std::string error;
};

ViewDump dump_activity_top(const std::string& adb_path,
                           const std::string& serial,
                           std::chrono::milliseconds timeout,
                           const CancellationToken& cancel);

}  // namespace mpi::android
