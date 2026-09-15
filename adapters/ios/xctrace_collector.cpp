#include "adapters/ios/xctrace_collector.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

#include "core/ingestion/reader.hpp"
#include "core/util/process.hpp"

namespace mpi::ios {
namespace {

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

model::Capability make_source(const char* id, const char* human,
                              model::CapabilityStatus status,
                              const char* provider) {
  model::Capability c;
  c.id = id;
  c.human_name = human;
  c.status = status;
  c.provider = provider;
  return c;
}

// A scratch directory for the bundle. Under the system temp root rather than
// the session package: the bundle is an intermediate, and the session keeps
// the exported XML, which is the format this tool can actually read back.
std::filesystem::path scratch_dir(const std::string& session_hint,
                                  std::string& error) {
  std::error_code ec;
  auto base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    error = "no writable temporary directory: " + ec.message();
    return {};
  }
  auto dir = base / ("mpi-xctrace-" + session_hint);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "cannot create " + dir.string() + ": " + ec.message();
    return {};
  }
  return dir;
}

}  // namespace

RecordOutcome interpret_record_output(const std::string& stdout_text,
                                      const std::string& stderr_text,
                                      int exit_code, bool timed_out) {
  RecordOutcome out;
  const std::string both = stdout_text + "\n" + stderr_text;

  out.attached = contains(both, "Starting recording") ||
                 contains(both, "Attaching to:");

  // xctrace announces the file it wrote, and it is not always the path that
  // was requested -- a relative --output lands in the working directory.
  const auto saved = both.find("Output file saved as:");
  if (saved != std::string::npos) {
    const auto start = saved + std::string("Output file saved as:").size();
    auto end = both.find('\n', start);
    if (end == std::string::npos) end = both.size();
    std::string path = both.substr(start, end - start);
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) {
      path.erase(path.begin());
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\r' ||
                             path.back() == '\n')) {
      path.pop_back();
    }
    out.output_path = path;
    out.wrote_bundle = !path.empty();
  }

  if (timed_out) {
    out.timed_out = true;
    // The measured failure mode on this host: attached, then never finished.
    out.refusal =
        out.attached
            ? "`xctrace record` attached to the target and did not finish "
              "within the time budget. Measured against a booted simulator on "
              "this host, where it attaches and then runs indefinitely even "
              "with --time-limit and --no-prompt. No recording was produced, "
              "which is not the same as a recording that found nothing"
            : "`xctrace record` did not finish within the time budget and "
              "never reported attaching to the target";
    return out;
  }

  if (contains(both, "Cannot find process")) {
    out.refusal =
        "xctrace could not find the process to attach to. For a simulator the "
        "pid is a host process id and the device must still be named with "
        "--device, so the pair has to match";
    return out;
  }
  if (contains(both, "Recording failed with errors")) {
    // It says this and still writes a usable bundle, so the bundle is kept
    // and the errors are carried alongside rather than discarding real data.
    out.notes.push_back(
        "xctrace reported 'Recording failed with errors' and still wrote a "
        "bundle; what it managed to record is kept, and this note travels "
        "with it");
    if (!out.wrote_bundle) {
      out.refusal = "xctrace reported recording errors and wrote no bundle";
    }
    return out;
  }
  if (exit_code != 0 && !out.wrote_bundle) {
    out.refusal = "`xctrace record` exited " + std::to_string(exit_code) +
                  " without writing a bundle";
  }
  return out;
}

std::string export_xpath_for(const std::string& schema, int run_number) {
  return "/trace-toc/run[@number=\"" + std::to_string(run_number) +
         "\"]/data/table[@schema=\"" + schema + "\"]";
}

session::CaptureResult XctraceCollector::capture(
    const model::DeviceRef& device,
    const std::vector<model::ProcessInstance>& processes,
    const session::CaptureConfig& config, model::NormalizedTrace& out) {
  const auto started_at = std::chrono::steady_clock::now();
  session::CaptureResult result;
  result.started = true;

  const auto finish = [&](model::CapabilityStatus status,
                          const std::string& evidence,
                          std::vector<std::string> limitations = {}) {
    auto c = make_source("ios.capture.live_recording",
                         "Live trace capture via Instruments", status,
                         "xctrace");
    c.evidence = evidence;
    c.limitations = std::move(limitations);
    // Whatever discovery established about this device, not a guess from the
    // trace: Instruments does not distinguish hardware from a simulator.
    c.tested = device.form == model::DeviceForm::kPhysical
                   ? model::TestedState::kVerifiedOnPhysicalDevice
                   : (device.form == model::DeviceForm::kSimulator
                          ? model::TestedState::kVerifiedOnSimulatorOrEmulator
                          : model::TestedState::kNotTested);
    result.source_results.push_back(std::move(c));
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    return result;
  };

  if (processes.empty()) {
    result.error = "no process instance was supplied; nothing to capture";
    return finish(model::CapabilityStatus::kUnknown, result.error);
  }
  const model::ProcessInstance* primary = &processes.front();
  for (const auto& p : processes) {
    if (p.is_primary) primary = &p;
  }
  const std::string pid = std::to_string(primary->pid);
  if (!proc::is_safe_argument(device.capture_id(), /*reject_option_like=*/true) ||
      !proc::is_safe_argument(pid, /*reject_option_like=*/true)) {
    result.error = "refusing to pass '" + device.device_id + "' / '" + pid +
                   "' to xctrace: it would be read as a command-line option";
    return finish(model::CapabilityStatus::kUnknown, result.error);
  }

  std::string scratch_error;
  const auto dir = scratch_dir(out.session_id.empty() ? pid : out.session_id,
                               scratch_error);
  if (!scratch_error.empty()) {
    result.error = scratch_error;
    return finish(model::CapabilityStatus::kUnknown, result.error);
  }
  const auto bundle = dir / "run.trace";
  std::error_code ec;
  std::filesystem::remove_all(bundle, ec);

  // The recording is bounded by the collector, not only by --time-limit:
  // xctrace has been observed to ignore its own limit and keep running.
  const auto window = config.duration.count() > 0
                          ? config.duration
                          : std::chrono::milliseconds(5000);
  proc::Options po;
  po.timeout = window + std::chrono::milliseconds(45000);
  po.cancel = config.cancel;

  const std::vector<std::string> record_argv = {
      "xcrun",   "xctrace",   "record",
      "--no-prompt",  // never wait on a prompt: there is no operator here
      // The hardware UDID, not the CoreDevice id: xctrace does not know
      // the latter. See DeviceRef::capture_id().
      "--device", device.capture_id(),
      "--template", "Time Profiler",
      "--attach", pid,
      "--output", bundle.string(),
      "--time-limit", std::to_string(window.count()) + "ms"};

  const auto recorded = proc::run(record_argv, po);
  if (!recorded.spawned) {
    result.error = "cannot run xctrace: " + recorded.spawn_error;
    return finish(model::CapabilityStatus::kUnsupported, result.error,
                  {"Instruments is part of Xcode; a Command Line Tools-only "
                   "install does not provide xctrace"});
  }

  const auto outcome = interpret_record_output(recorded.out, recorded.err,
                                               recorded.exit_code,
                                               recorded.timed_out);
  if (recorded.cancelled) {
    out.partial = true;
    out.partial_reasons.push_back("capture cancelled by the operator");
    result.error = "capture cancelled";
    return finish(model::CapabilityStatus::kUnknown, result.error);
  }
  if (!outcome.refusal.empty()) {
    result.error = outcome.refusal;
    return finish(model::CapabilityStatus::kUnsupported, outcome.refusal,
                  {"no trace was produced, so nothing about the app was "
                   "measured; this is a provider failure and not an absence "
                   "of findings"});
  }

  const std::string bundle_path =
      outcome.output_path.empty() ? bundle.string() : outcome.output_path;
  if (!std::filesystem::exists(bundle_path)) {
    result.error = "xctrace reported success but no trace bundle exists at " +
                   bundle_path;
    return finish(model::CapabilityStatus::kUnsupported, result.error);
  }

  // Export the one table this tool reads, then read it. The bundle itself is
  // never parsed: it is an undocumented package.
  const auto exported = dir / "time-profile.xml";
  const auto export_result = proc::run(
      {"xcrun", "xctrace", "export", "--input", bundle_path, "--xpath",
       export_xpath_for("time-profile")},
      po);
  if (!export_result.ok()) {
    result.error =
        "`xctrace export` failed: " +
        (export_result.spawned ? export_result.err : export_result.spawn_error);
    return finish(model::CapabilityStatus::kLimited, result.error,
                  {"the recording exists at " + bundle_path +
                   " but could not be exported, so none of it could be read"});
  }
  {
    // proc::run captures stdout, and the export can be large, so it goes
    // straight to a file for the reader to stream.
    FILE* f = std::fopen(exported.string().c_str(), "wb");
    if (f == nullptr) {
      result.error = "cannot write " + exported.string();
      return finish(model::CapabilityStatus::kLimited, result.error);
    }
    std::fwrite(export_result.out.data(), 1, export_result.out.size(), f);
    std::fclose(f);
  }

  ingest::ReadOptions read_opts;
  read_opts.cancel = config.cancel;
  ingest::ReadDiagnostics diag;
  ingest::XctraceExportReader reader;
  const bool read_ok = reader.read(exported.string(), read_opts, out, diag);
  for (const auto& w : diag.warnings) out.ingestion_warnings.push_back(w);
  for (const auto& n : outcome.notes) out.ingestion_warnings.push_back(n);
  if (!read_ok) {
    result.error = diag.errors.empty()
                       ? "the exported table held no samples"
                       : diag.errors.front();
    return finish(model::CapabilityStatus::kLimited, result.error,
                  {"the recording and the export both succeeded, so this is "
                   "an empty or unreadable table rather than a missing "
                   "provider"});
  }

  result.any_data = !out.cpu_samples.empty() || !out.events.empty();
  return finish(model::CapabilityStatus::kAvailable,
                "recorded with the Time Profiler template and exported " +
                    std::to_string(out.cpu_samples.size()) + " sample(s)",
                {"Instruments records a fixed window and writes its bundle at "
                 "the end, so there is no live view of a capture in progress",
                 "the export carries no bundle identifier, so the target is "
                 "pinned by process name"});
}

}  // namespace mpi::ios
