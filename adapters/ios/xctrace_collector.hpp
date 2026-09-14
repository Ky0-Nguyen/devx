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
#include <string>
#include <vector>

#include "core/session/collector.hpp"

namespace mpi::ios {

// What one `xctrace record` invocation did, read from its own output rather
// than from its exit status alone: it exits non-zero for a partial recording
// that still produced a usable bundle.
struct RecordOutcome {
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
