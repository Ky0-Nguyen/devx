#include "core/observe/screenshot.hpp"

#include <chrono>
#include <fstream>

#include "core/util/process.hpp"

namespace mpi::observe {
namespace {

std::int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

const char* to_string(ShotMoment m) {
  switch (m) {
    case ShotMoment::kBeforeCapture: return "before_capture";
    case ShotMoment::kDuringCapture: return "during_capture";
    case ShotMoment::kAfterCapture:  return "after_capture";
    case ShotMoment::kUnrelated:     return "unrelated";
  }
  return "unrelated";
}

const char* evidence_note(ShotMoment m) {
  switch (m) {
    case ShotMoment::kBeforeCapture:
      return "the screen before collection started: it shows where the app "
             "began, not what any finding refers to";
    case ShotMoment::kDuringCapture:
      return "the screen at this offset into the capture: it is one instant "
             "out of the whole window, and is not the frame any detector "
             "flagged unless their timestamps coincide";
    case ShotMoment::kAfterCapture:
      return "the screen after collection ended: it shows where the app "
             "finished, and is not evidence about any earlier moment";
    case ShotMoment::kUnrelated:
      return "taken outside any capture window: it dates the image and "
             "nothing else";
  }
  return "";
}

bool png_dimensions(const std::string& bytes, int* width, int* height) {
  // 8-byte signature, then a 4-byte length, "IHDR", then width and height as
  // big-endian 32-bit values: 24 bytes before anything can be read.
  static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G',
                                              0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 24) return false;
  for (int i = 0; i < 8; i++) {
    if (static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]) !=
        kSignature[i]) {
      return false;
    }
  }
  if (bytes.compare(12, 4, "IHDR") != 0) return false;
  auto be32 = [&bytes](std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
  };
  const std::uint32_t w = be32(16);
  const std::uint32_t h = be32(20);
  // A PNG cannot legally be zero in either dimension, and an absurd one is a
  // truncated or mangled stream rather than a very large picture.
  if (w == 0 || h == 0 || w > 65535 || h > 65535) return false;
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

Screenshot capture_screen(const ShotOptions& options) {
  Screenshot shot;
  shot.moment = options.moment;
  shot.path = options.out_path;

  if (!proc::is_safe_argument(options.device_id, true) ||
      !proc::is_safe_argument(options.out_path, true)) {
    shot.error = "the device id or output path is not a safe argument";
    return shot;
  }

  proc::Options run_opts;
  // Generous, because a screenshot on a cold emulator is slow, but bounded.
  run_opts.timeout = std::chrono::milliseconds(20000);
  run_opts.cancel = options.cancel;

  std::string bytes;
  if (options.platform == model::Platform::kAndroid) {
    shot.basis = "adb exec-out screencap -p";
    // `exec-out` rather than `shell`: `shell` runs through a pty that
    // translates \n to \r\n, which corrupts every PNG it carries.
    const proc::Result r = proc::run(
        {"adb", "-s", options.device_id, "exec-out", "screencap", "-p"},
        run_opts);
    if (!r.spawned) {
      shot.error = "adb could not be started: " + r.spawn_error;
      return shot;
    }
    if (r.timed_out) {
      shot.error = "screencap did not finish within 20s";
      return shot;
    }
    if (r.exit_code != 0) {
      shot.error = "screencap failed: " + r.err;
      return shot;
    }
    bytes = r.out;
  } else {
    // A simulator udid is what `simctl` accepts. There is no equivalent for a
    // physical device, and the caller is told so rather than handed an empty
    // file.
    shot.basis = "xcrun simctl io <udid> screenshot";
    const proc::Result r = proc::run(
        {"xcrun", "simctl", "io", options.device_id, "screenshot",
         options.out_path}, run_opts);
    if (!r.spawned) {
      shot.error = "xcrun could not be started: " + r.spawn_error;
      return shot;
    }
    if (r.exit_code != 0) {
      shot.error = "simctl screenshot failed: " + r.err +
                   " (a physical iOS device has no command-line screenshot; "
                   "this works for simulators only)";
      return shot;
    }
    std::ifstream in(options.out_path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    bytes = std::move(data);
  }

  if (bytes.empty()) {
    shot.error = "the screenshot command succeeded and produced no bytes";
    return shot;
  }
  if (!png_dimensions(bytes, &shot.width, &shot.height)) {
    // Refusing here matters: a truncated or pty-mangled stream is still a
    // file, and writing it would put a broken image in a report.
    shot.error = "the output was not a valid PNG (" +
                 std::to_string(bytes.size()) + " bytes)";
    return shot;
  }

  if (options.platform == model::Platform::kAndroid) {
    std::ofstream out(options.out_path, std::ios::binary);
    if (!out) {
      shot.error = "could not write " + options.out_path;
      return shot;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      shot.error = "the write to " + options.out_path + " did not complete";
      return shot;
    }
  }

  shot.captured = true;
  shot.bytes = static_cast<std::int64_t>(bytes.size());
  shot.taken_at_unix_ms = unix_ms_now();
  if (options.capture_start_unix_ms > 0) {
    shot.offset_into_capture_ns =
        (shot.taken_at_unix_ms - options.capture_start_unix_ms) * 1'000'000;
  }
  return shot;
}

json::Value Screenshot::to_json() const {
  json::Value v = json::Value::object();
  v.set("captured", json::Value::boolean(captured));
  v.set("path", json::Value::string(path));
  v.set("bytes", json::Value::integer(bytes));
  // Dimensions only when there is an image: 0x0 would read as a real size.
  v.set("width", captured ? json::Value::integer(width) : json::Value::null());
  v.set("height", captured ? json::Value::integer(height) : json::Value::null());
  v.set("taken_at_unix_ms", taken_at_unix_ms > 0
                                ? json::Value::integer(taken_at_unix_ms)
                                : json::Value::null());
  v.set("offset_into_capture_ns",
        offset_into_capture_ns >= 0
            ? json::Value::integer(offset_into_capture_ns)
            : json::Value::null());
  v.set("moment", json::Value::string(to_string(moment)));
  // The note travels with the image. A reader who sees only the JSON, or only
  // the picture in a document, still gets told what it is evidence of.
  v.set("shows", json::Value::string(evidence_note(moment)));
  v.set("basis", json::Value::string(basis));
  if (!error.empty()) v.set("error", json::Value::string(error));
  return v;
}

}  // namespace mpi::observe
