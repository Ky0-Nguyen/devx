// iOS capture over `xcrun xctrace`.
//
// Instruments is the only sanctioned profiler for an iOS app, and its only
// supported machine interface is the pair
//
//   xctrace record  ...  --output run.trace
//   xctrace export  --input run.trace --xpath '<table>'
//
// so a capture here is: record, then export, then read the XML through
// `ios.xctrace.export`. The `.trace` bundle in between is an undocumented
// package and is never parsed directly.
//
// Two honesty problems come with this provider and are handled explicitly.
//
// First, `record` can attach and then never finish. Measured on this host
// against a booted simulator: it printed "Attaching to: Settings (87700)" and
// was still running minutes later with `--time-limit 3s --no-prompt`. So the
// recording is bounded by the collector and a run that overruns is reported as
// a provider that did not deliver -- not as a capture that found nothing.
//
// Second, Instruments does not say whether it recorded hardware or a
// simulator. The device form therefore comes from discovery, which does know,
// and is never inferred from the trace (spec J18).
#pragma once

#include <optional>
#include <chrono>
#include <string>
#include <vector>

#include "core/util/process.hpp"
#include "core/session/collector.hpp"

namespace mpi::ios {

// What one `xctrace record` invocation did, read from its own output rather
// than from its exit status alone: it exits non-zero for a partial recording
// that still produced a usable bundle.
/// How a recording is stopped when it runs past its budget.
///
/// Named rather than inline so a test can assert that this collector asks for
/// them: `proc::run` honouring a stop signal is one thing, and this collector
/// actually choosing the right one is another, and only the second prevents
/// the data loss. Instruments finalises its trace bundle on **SIGINT** -- it
/// prints "Ctrl-C to stop the recording" -- and needs seconds to write it, so
/// the default SIGTERM-then-SIGKILL-in-500ms destroyed captures that had
/// already succeeded.
/// Whether a trace bundle can actually be read.
///
/// The question a timeout must not answer by itself. `xctrace record` can
/// finish a recording and then fail to exit, and refusing on the exit code
/// threw away real captures; equally, it can leave a **stub** bundle that
/// looks like a result and is not. Asking `xctrace export --toc` settles it:
/// a stub is rejected with "Document Missing Template Error" in well under a
/// second, and a real bundle answers with a `<trace-toc>` document.
///
/// Extracted from the capture path so it can be tested. Inline, it was
/// reachable only with a device attached, which is the one thing this
/// environment has never had.
struct BundleReadability {
  bool readable = false;
  /// Why not, in the tool's own words, when it is not.
  std::string detail;
};
BundleReadability bundle_is_readable(const std::string& bundle_path,
                                     const proc::Options& opts);

int xctrace_stop_signal();
std::chrono::milliseconds xctrace_stop_grace();

struct RecordOutcome {
  /// Whether xctrace got as far as actually recording.
  ///
  /// It prints "Ctrl-C to stop the recording" at that point, and the absence
  /// of that line while still holding the target is the signature of the
  /// simulator hang: accepted, never recording, unresponsive to SIGINT.
  bool began_recording = false;
  bool attached = false;
  bool wrote_bundle = false;
  bool timed_out = false;
  // The path xctrace reported writing, which is not always the one asked for.
  std::string output_path;
  std::string refusal;
  std::vector<std::string> notes;
};

// Interprets `xctrace record` output. Exposed for testing: the interesting
// cases are failures, and they are cheaper to pin as text than to reproduce.
RecordOutcome interpret_record_output(const std::string& stdout_text,
                                      const std::string& stderr_text,
                                      int exit_code, bool timed_out);

// The xpath for one table schema in a run. Built here so the quoting lives in
// one place and cannot drift between call sites.
std::string export_xpath_for(const std::string& schema, int run_number = 1);

class XctraceCollector final : public session::Collector {
 public:
  std::string id() const override { return "ios.xctrace"; }
  model::Platform platform() const override { return model::Platform::kIos; }

  session::CaptureResult capture(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

  // Instruments records for a fixed window and writes a bundle at the end.
  // There is no incremental read, so there is nothing to stream: claiming
  // otherwise would mean inventing intermediate numbers.
  bool supports_streaming() const override { return false; }
};

}  // namespace mpi::ios
