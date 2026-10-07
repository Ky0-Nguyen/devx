#include "adapters/android/emulator/emulator_control.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "core/util/process.hpp"
#include "core/util/protobuf.hpp"

namespace mpi::android {
namespace {

const std::string kController = "/android.emulation.control.EmulatorController/";
const std::string kUi = "/android.emulation.control.UiController/";

// ImageFormat (emulator_controller.proto): 1 format, 3 width, 4 height,
// 6 transport { 1 channel (MMAP = 1), 2 handle }.
std::string image_format(int format, std::uint32_t w, std::uint32_t h,
                         const std::string& mmap_handle) {
  pb::Writer f;
  f.varint(1, static_cast<std::uint64_t>(format));
  if (w > 0) f.varint(3, w);
  if (h > 0) f.varint(4, h);
  if (!mmap_handle.empty()) {
    pb::Writer t;
    t.varint(1, 1);
    t.string(2, mmap_handle);
    f.message(6, t);
  }
  return f.data();
}

}  // namespace

EmulatorControl::~EmulatorControl() {
  stop_stream();
  if (ch_) ch_->close();
}

bool EmulatorControl::connect(const RunningEmulator& emulator, std::string* error) {
  if (!emulator.attachable()) {
    if (error != nullptr) *error = "this emulator has no gRPC token DevX can use";
    return false;
  }
  emu_ = emulator;
  ch_ = std::make_unique<net::GrpcChannel>(static_cast<std::uint16_t>(emulator.grpc_port),
                                           "Bearer " + emulator.token);
  return ch_->connect(error);
}

bool EmulatorControl::alive() const { return ch_ && ch_->alive(); }

bool EmulatorControl::call(const std::string& method, const std::string& request,
                           std::string* response) {
  if (!ch_) {
    last_error_ = "not connected";
    return false;
  }
  std::string ignored;
  const auto st = ch_->unary(method, request, response != nullptr ? response : &ignored,
                             std::chrono::seconds(10));
  if (!st.ok()) {
    last_error_ = method.substr(method.rfind('/') + 1) + ": " +
                  (st.message.empty() ? "gRPC status " + std::to_string(st.code) : st.message);
    return false;
  }
  return true;
}

bool EmulatorControl::status(json::Value* out, std::string* error) {
  std::string resp;
  if (!call(kController + "getStatus", "", &resp)) {
    if (error != nullptr) *error = last_error_;
    return false;
  }
  // EmulatorStatus: 1 version, 2 uptime, 3 booted, 5 hardwareConfig { 1 entry { 1 key, 2 value } }.
  json::Value o = json::Value::object();
  json::Value hw = json::Value::object();
  pb::Reader r(resp);
  pb::Field f;
  while (r.next(f)) {
    if (f.number == 1) o.set("version", json::Value::string(std::string(f.bytes)));
    if (f.number == 2) o.set("uptime_ms", json::Value::integer(static_cast<std::int64_t>(f.value)));
    if (f.number == 3) o.set("booted", json::Value::boolean(f.value != 0));
    if (f.number == 5) {
      pb::Reader list(f.bytes);
      pb::Field e;
      while (list.next(e)) {
        if (e.number != 1) continue;
        pb::Reader kv(e.bytes);
        pb::Field x;
        std::string k, v;
        while (kv.next(x)) {
          if (x.number == 1) k = std::string(x.bytes);
          if (x.number == 2) v = std::string(x.bytes);
        }
        if (k.rfind("hw.lcd.", 0) == 0 || k == "hw.ramSize" || k == "avd.name" ||
            k == "hw.cpu.ncore" || k == "hw.device.name") {
          hw.set(k, json::Value::string(v));
        }
      }
    }
  }
  o.set("hardware", std::move(hw));
  o.set("rotation", json::Value::integer(rotation_));
  if (out != nullptr) *out = std::move(o);
  return true;
}

bool EmulatorControl::screenshot_png(std::string* png, int max_width, int max_height,
                                     std::string* error) {
  std::string resp;
  if (!call(kController + "getScreenshot",
            image_format(0, static_cast<std::uint32_t>(std::max(0, max_width)),
                         static_cast<std::uint32_t>(std::max(0, max_height)), ""),
            &resp)) {
    if (error != nullptr) *error = last_error_;
    return false;
  }
  pb::Reader r(resp);
  pb::Field f;
  while (r.next(f)) {
    if (f.number == 4) {  // Image.image
      png->assign(f.bytes.data(), f.bytes.size());
      return !png->empty();
    }
  }
  if (error != nullptr) *error = "the emulator returned no image (is the display on?)";
  return false;
}

bool EmulatorControl::touch(int x, int y, int pressure, int pointer_id) {
  // TouchEvent { 1 touches: Touch { 1 x, 2 y, 3 identifier, 4 pressure } }.
  pb::Writer t;
  t.int32(1, x);
  t.int32(2, y);
  t.int32(3, pointer_id);
  t.int32(4, pressure);
  pb::Writer ev;
  ev.message(1, t);
  return call(kController + "sendTouch", ev.data(), nullptr);
}

bool EmulatorControl::key(const std::string& key, int phase) {
  // KeyboardEvent { 2 eventType (keydown 0, keyup 1, keypress 2), 4 key }.
  pb::Writer k;
  k.varint(2, static_cast<std::uint64_t>(phase));
  k.string(4, key);
  return call(kController + "sendKey", k.data(), nullptr);
}

bool EmulatorControl::text(const std::string& utf8) {
  pb::Writer k;
  k.varint(2, 2);
  k.string(5, utf8);  // KeyboardEvent.text
  return call(kController + "sendKey", k.data(), nullptr);
}

bool EmulatorControl::rotate(int degrees) {
  degrees = ((degrees % 360) + 360) % 360;
  // PhysicalModelValue { 1 target (ROTATION = 1), 3 value: ParameterValue { 1 data } }.
  pb::Writer v;
  v.flt(1, 0);
  v.flt(1, 0);
  v.flt(1, static_cast<float>(degrees));
  pb::Writer m;
  m.varint(1, 1);
  m.message(3, v);
  if (!call(kController + "setPhysicalModel", m.data(), nullptr)) return false;
  rotation_ = degrees;
  return true;
}

bool EmulatorControl::show_extended_controls(int pane) {
  pb::Writer p;
  if (pane > 0) p.varint(1, static_cast<std::uint64_t>(pane));  // PaneEntry.index
  return call(kUi + "showExtendedControls", p.data(), nullptr);
}

bool EmulatorControl::set_battery(int level_percent, bool charging) {
  // BatteryState { 1 hasBattery, 2 isPresent, 3 charger (AC = 1, NONE = 0),
  // 4 chargeLevel, 5 health (GOOD = 0), 6 status (CHARGING 1, NOT_CHARGING 3) }.
  pb::Writer b;
  b.boolean(1, true);
  b.boolean(2, true);
  b.varint(3, charging ? 1 : 0);
  b.int32(4, std::max(0, std::min(100, level_percent)));
  b.varint(5, 0);
  b.varint(6, charging ? 1 : 3);
  return call(kController + "setBattery", b.data(), nullptr);
}

bool EmulatorControl::set_gps(double latitude, double longitude) {
  // GpsState { 1 passiveUpdate, 2 latitude, 3 longitude, 4 speed, 5 bearing,
  // 6 altitude, 7 satellites }.
  pb::Writer g;
  g.boolean(1, false);
  g.dbl(2, latitude);
  g.dbl(3, longitude);
  g.dbl(6, 0);
  g.int32(7, 4);
  return call(kController + "setGps", g.data(), nullptr);
}

bool EmulatorControl::start_stream(const std::string& mmap_path, std::uint32_t box,
                                   std::function<void(const FrameInfo&)> on_frame,
                                   std::string* error) {
  stop_stream();
  if (!ch_) {
    if (error != nullptr) *error = "not connected";
    return false;
  }
  const int fd = ::open(mmap_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(box) * box * 4) != 0) {
    if (fd >= 0) ::close(fd);
    if (error != nullptr) *error = "could not create the frame file " + mmap_path;
    return false;
  }
  ::close(fd);
  stream_ = ch_->server_stream(
      kController + "streamScreenshot", image_format(1, box, box, "file://" + mmap_path),
      [cb = std::move(on_frame)](std::string_view msg) {
        // Image { 1 format { 3 width, 4 height }, 5 seq, 6 timestampUs }.
        FrameInfo fi;
        pb::Reader r(msg);
        pb::Field f;
        while (r.next(f)) {
          if (f.number == 1) {
            pb::Reader fr(f.bytes);
            pb::Field x;
            while (fr.next(x)) {
              if (x.number == 3) fi.width = static_cast<std::uint32_t>(x.value);
              if (x.number == 4) fi.height = static_cast<std::uint32_t>(x.value);
            }
          }
          if (f.number == 5) fi.seq = static_cast<std::uint32_t>(f.value);
          if (f.number == 6) fi.timestamp_us = f.value;
        }
        if (cb) cb(fi);
      });
  if (stream_->finished()) {
    if (error != nullptr) *error = stream_->status().message;
    return false;
  }
  return true;
}

void EmulatorControl::stop_stream() {
  if (stream_) {
    stream_->cancel();
    stream_.reset();
  }
}

bool override_screen(const std::string& sdk_root, const std::string& serial, int width,
                     int height, int density, std::string* error) {
  const std::string adb = sdk_root + "/platform-tools/adb";
  if (!proc::is_safe_argument(serial, true)) {
    if (error != nullptr) *error = "refusing a serial that would be read as an option";
    return false;
  }
  proc::Options po;
  po.timeout = std::chrono::seconds(30);
  // Android reports booted over gRPC before the adb server has picked the
  // device up; `wm` straight after a start failed with "device not found".
  proc::run({adb, "-s", serial, "wait-for-device"}, po);
  po.timeout = std::chrono::seconds(15);
  auto shell = [&](const std::vector<std::string>& args) {
    std::vector<std::string> argv = {adb, "-s", serial, "shell"};
    argv.insert(argv.end(), args.begin(), args.end());
    const auto r = proc::run(argv, po);
    if (!r.ok() && error != nullptr) {
      *error = r.spawned ? (r.err.empty() ? r.out : r.err) : r.spawn_error;
    }
    return r.ok();
  };
  const bool size_ok = width > 0 && height > 0
                           ? shell({"wm", "size", std::to_string(width) + "x" + std::to_string(height)})
                           : shell({"wm", "size", "reset"});
  const bool density_ok = density > 0 ? shell({"wm", "density", std::to_string(density)})
                                      : shell({"wm", "density", "reset"});
  return size_ok && density_ok;
}

}  // namespace mpi::android
