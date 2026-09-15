// A picture of the screen, and an honest statement of when it was taken.
//
// A screenshot in a report is genuinely useful: "22.5% of frames missed their
// deadline" says nothing about *which* screen, and the reader almost always
// knows the app well enough to recognise it instantly.
//
// But it is also the easiest place in this whole tool to imply something
// false, so the rule is structural rather than advisory:
//
//   **A screenshot shows the screen at the moment it was taken, and nothing
//   else.** It is not the frame that missed its deadline. A trace analysed
//   tomorrow cannot be photographed today, and a capture that ran for ten
//   seconds has one image out of six hundred frames.
//
// So every screenshot carries when it was taken and what it is therefore
// evidence *of*. A picture taken after a capture ended is labelled as the
// screen afterwards -- which is a statement about where the app finished, and
// no statement at all about where it was when the detector fired.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/identity.hpp"
#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::observe {

/// What a screenshot is evidence of, which depends entirely on when it was
/// taken relative to the thing being reported.
enum class ShotMoment {
  kBeforeCapture,  // taken before collection started
  kDuringCapture,  // taken while collecting; the timestamp places it
  kAfterCapture,   // taken once collection ended
  kUnrelated,      // taken at some other time; it dates the image, not an event
};
const char* to_string(ShotMoment m);
/// The sentence that must accompany an image taken at this moment.
const char* evidence_note(ShotMoment m);

struct Screenshot {
  bool captured = false;
  std::string path;
  std::int64_t bytes = 0;
  int width = 0;
  int height = 0;
  /// Wall-clock time, so the image can be placed against a capture's window.
  std::int64_t taken_at_unix_ms = 0;
  /// Nanoseconds from the start of the capture, when there was one.
  std::int64_t offset_into_capture_ns = -1;
  ShotMoment moment = ShotMoment::kUnrelated;
  /// The command that produced it, named so the reader knows what took it.
  std::string basis;
  std::string error;

  json::Value to_json() const;
};

struct ShotOptions {
  model::Platform platform = model::Platform::kAndroid;
  std::string device_id;
  /// Where to write the PNG. The directory must exist.
  std::string out_path;
  ShotMoment moment = ShotMoment::kUnrelated;
  std::int64_t capture_start_unix_ms = 0;
  CancellationToken cancel;
};

/// Takes one screenshot.
///
/// Android goes through `adb exec-out screencap -p`, which writes a PNG to
/// stdout. iOS simulators go through `xcrun simctl io <udid> screenshot`,
/// which writes the file itself. A **physical** iOS device has no route here
/// and is reported unavailable rather than attempted: there is no supported
/// command-line screenshot for one, and pretending otherwise would produce an
/// empty file where a reader expects a picture.
Screenshot capture_screen(const ShotOptions& options);

/// Reads a PNG's dimensions from its IHDR chunk.
///
/// Separate and pure so it can be tested on bytes. Returns false for anything
/// that is not a PNG -- including an empty file, which is what a failed
/// capture leaves behind and must not be reported as a 0x0 image.
bool png_dimensions(const std::string& bytes, int* width, int* height);

}  // namespace mpi::observe
