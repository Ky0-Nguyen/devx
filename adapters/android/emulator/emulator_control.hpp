// Driving a running emulator over its gRPC API
// (emulator/lib/emulator_controller.proto and ui_controller_service.proto in
// the SDK; every field number below is from those files).
//
// The screen stream uses the proto's MMAP transport: the client owns a file,
// the emulator writes each frame into it, and the stream itself carries only a
// few bytes of metadata per frame. Measured on an Apple Silicon Mac against a
// 1080x2400 device scaled to 540x1200: ~47 frames a second, 38 KB over the
// socket in five seconds against 539 MB when the pixels travel in the stream.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "adapters/android/emulator/emulator_process.hpp"
#include "core/net/grpc_client.hpp"
#include "core/util/json.hpp"

namespace mpi::android {

struct FrameInfo {
  std::uint32_t seq = 0;
  std::uint32_t width = 0;   // of the image in the shared file, RGBA8888
  std::uint32_t height = 0;
  std::uint64_t timestamp_us = 0;
};

class EmulatorControl {
 public:
  EmulatorControl() = default;
  ~EmulatorControl();

  bool connect(const RunningEmulator& emulator, std::string* error);
  bool alive() const;
  const RunningEmulator& emulator() const { return emu_; }

  /// getStatus: version, uptime, booted, and the hardware configuration.
  bool status(json::Value* out, std::string* error);
  /// One PNG, scaled to fit `max_width` x `max_height` (0 for full size).
  bool screenshot_png(std::string* png, int max_width, int max_height, std::string* error);

  /// Touch at device pixels. `pressure` 0 lifts the finger.
  bool touch(int x, int y, int pressure, int pointer_id = 0);
  /// A W3C key value: "GoHome", "GoBack", "AppSwitch", "Power", "AudioVolumeUp",
  /// "Enter", "Backspace", or a printable character. `phase`: 0 down, 1 up,
  /// 2 press.
  bool key(const std::string& key, int phase = 2);
  bool text(const std::string& utf8);
  /// Rotates the device to 0, 90, 180 or 270 degrees.
  bool rotate(int degrees);
  int rotation() const { return rotation_; }
  /// Opens the emulator's own Extended Controls window at a pane (0 keeps the
  /// current one; 1 location, 4 battery, 5 camera, 6 phone ...).
  bool show_extended_controls(int pane);
  bool set_battery(int level_percent, bool charging);
  bool set_gps(double latitude, double longitude);

  /// Streams the screen into `mmap_path`, which this call sizes to
  /// `box * box * 4` bytes so either orientation fits. `on_frame` runs on the
  /// connection's reader thread.
  bool start_stream(const std::string& mmap_path, std::uint32_t box,
                    std::function<void(const FrameInfo&)> on_frame, std::string* error);
  void stop_stream();
  /// False once the screen stream has ended: the emulator stopped or
  /// restarted, or the connection dropped.
  bool stream_alive() const { return stream_ && !stream_->finished(); }

  const std::string& last_error() const { return last_error_; }

 private:
  bool call(const std::string& method, const std::string& request, std::string* response);

  RunningEmulator emu_;
  std::unique_ptr<net::GrpcChannel> ch_;
  std::shared_ptr<net::GrpcStream> stream_;
  int rotation_ = 0;
  std::string last_error_;
};

/// Overrides the screen without a restart, through `adb shell wm`. Width,
/// height and density of 0 reset to the device's own.
bool override_screen(const std::string& sdk_root, const std::string& serial, int width,
                     int height, int density, std::string* error);

}  // namespace mpi::android
