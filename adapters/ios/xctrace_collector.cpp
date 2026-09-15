#include "adapters/ios/xctrace_collector.hpp"

#include <chrono>
#include <cstdio>
#include <csignal>
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

BundleReadability bundle_is_readable(const std::string& bundle_path,
                                     const proc::Options& opts) {
  BundleReadability out;
  if (bundle_path.empty()) {
    out.detail = "no bundle path";
    return out;
  }
  if (!std::filesystem::exists(bundle_path)) {
    // Distinct from "unreadable": nothing was written at all, which is a
    // different failure with a different cause.
    out.detail = "no bundle exists at " + bundle_path;
    return out;
  }
  if (!proc::is_safe_argument(bundle_path, /*reject_option_like=*/true)) {
    out.detail = "refusing to pass '" + bundle_path + "' to xctrace";
    return out;
  }
  const auto toc = proc::run(
      {"xcrun", "xctrace", "export", "--input", bundle_path, "--toc"}, opts);
  if (!toc.spawned) {
    out.detail = "could not run xctrace to check the bundle: " + toc.spawn_error;
    return out;
  }
  if (toc.timed_out) {
    out.detail = "the readability check did not finish, so whether the "
                 "bundle is usable is unknown";
    return out;
  }
  if (toc.exit_code != 0) {
    // "Document Missing Template Error" is what an unfinished recording
    // produces. The tool's own words are kept rather than paraphrased.
    out.detail = "xctrace cannot read the bundle: " +
                 (toc.err.empty() ? toc.out : toc.err);
    return out;
  }
  // A zero exit is not enough on its own: the table of contents has to be
  // there, or a future xctrace that succeeds while printing nothing would
  // read as a valid capture.
  if (toc.out.find("<trace-toc>") == std::string::npos) {
    out.detail = "xctrace exited successfully without producing a table of "
                 "contents, so there is nothing to read";
    return out;
  }
  out.readable = true;
  return out;
}

AlternateSampleTable find_unread_sample_table(const std::string& bundle_path,
                                              const proc::Options& opts) {
  AlternateSampleTable out;
  if (bundle_path.empty()) return out;
  if (!proc::is_safe_argument(bundle_path, /*reject_option_like=*/true)) {
    return out;
  }
  // Only tables that plausibly carry per-sample rows. Listed rather than
  // discovered from the table of contents, because "any table with rows" is
  // not the same claim: a kdebug table has rows in every trace and says
  // nothing about CPU sampling.
  static const char* kCandidates[] = {"time-sample", "cpu-profile",
                                      "counters-profile"};
  for (const char* schema : kCandidates) {
    const proc::Result r = proc::run(
        {"xcrun", "xctrace", "export", "--input", bundle_path, "--xpath",
         export_xpath_for(schema)}, opts);
    if (!r.spawned || r.exit_code != 0) continue;
    // Count rows without parsing: the point is whether there are any, and
    // parsing a schema this build does not support is exactly what is being
    // reported as missing.
    std::int64_t rows = 0;
    std::size_t at = 0;
    while ((at = r.out.find("<row>", at)) != std::string::npos) {
      rows++;
      at += 5;
    }
    if (rows > 0) {
      out.schema = schema;
      out.rows = rows;
      return out;
    }
  }
  return out;
}

LogStoreAccess probe_log_store_access() {
  LogStoreAccess out;
  proc::Options po;
  po.timeout = std::chrono::milliseconds(5000);
  // The smallest question that requires opening the store.
  const proc::Result r =
      proc::run({"/usr/bin/log", "show", "--last", "1s", "--style", "compact"},
                po);
  if (!r.spawned) {
    out.detail = "could not run `log`: " + r.spawn_error;
    return out;
  }
  if (r.timed_out) {
    out.detail = "`log show` did not answer within 5s";
    return out;
  }
  if (r.exit_code == 0) {
    out.readable = true;
    return out;
  }
  std::string message = r.err.empty() ? r.out : r.err;
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r' ||
          message.back() == ' ')) {
    message.pop_back();
  }
  out.detail = message;
  return out;
}

int xctrace_stop_signal() { return SIGINT; }
std::chrono::milliseconds xctrace_stop_grace() {
  return std::chrono::milliseconds(12000);
}

RecordOutcome interpret_record_output(const std::string& stdout_text,
                                      const std::string& stderr_text,
                                      int exit_code, bool timed_out) {
  RecordOutcome out;
  const std::string both = stdout_text + "\n" + stderr_text;

  out.attached = contains(both, "Starting recording") ||
                 contains(both, "Attaching to:");
  // The line that separates "it started recording" from "it is still trying
  // to attach". xctrace prints this once the recording is actually running,
  // and its absence is the signature of the simulator hang below.
  out.began_recording = contains(both, "Ctrl-C to stop the recording");

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
    if (out.attached && !out.began_recording) {
      // The measured signature of a broken simulator target: xctrace accepts
      // the target, never prints "Ctrl-C to stop the recording", never
      // reaches its own --time-limit, and does not respond to SIGINT. The
      // bundle it leaves is a stub that `xctrace export --toc` rejects with
      // "Document Missing Template Error".
      //
      // Isolated by comparison rather than assumed. Against a plain macOS
      // process the same command honours --time-limit, exits by itself and
      // writes a bundle that exports. Against a simulator target it hangs --
      // whether by --attach or --launch -- and it completes only when the
      // launch *fails* and there is nothing to record. So the fault is in
      // recording a simulator target, not in the invocation.
      out.refusal =
          "`xctrace record` accepted the target and never started recording: "
          "it did not print \"Ctrl-C to stop the recording\", did not reach "
          "its own --time-limit, and did not respond to SIGINT. Measured on "
          "this host, Instruments cannot record an iOS **simulator** target; "
          "the same command works against a macOS process. Use live capture "
          "instead, which reads the simulator app as a host process and needs "
          "no Instruments, or record on a physical device";
    } else if (out.attached) {
      // It did start recording, so there may be a real trace to salvage --
      // the caller tries the export before believing this.
      out.refusal =
          "`xctrace record` started recording and then did not exit within "
          "the time budget";
    } else {
      out.refusal =
          "`xctrace record` did not finish within the time budget and never "
          "reported accepting the target";
    }
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
  // 20s of slack rather than 45: the failure mode this guards
  // against is unbounded, so a longer budget buys nothing except a
  // longer wait before the diagnosis.
  po.timeout = window + std::chrono::milliseconds(20000);
  po.cancel = config.cancel;
  // Instruments finalises its trace bundle on SIGINT -- it prints "Ctrl-C to
  // stop the recording" -- and needs seconds to write it. The default
  // SIGTERM-then-SIGKILL-in-500ms killed a capture that had actually been
  // taken and reported it as a provider failure. Measured: xctrace honours
  // --time-limit and exits by itself against a macOS process, and against a
  // simulator process it attaches and never reaches its limit at all.
  po.stop_signal = xctrace_stop_signal();
  po.stop_grace = xctrace_stop_grace();

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
  const std::string bundle_path =
      outcome.output_path.empty() ? bundle.string() : outcome.output_path;

  // A refusal used to end the capture here. It does not any more, because one
  // of the refusals is "xctrace did not exit", and a recording it had already
  // finished was being thrown away on the strength of an exit code. The
  // question "is there a usable trace?" is answered by trying to read one,
  // not by how the process ended.
  //
  // `--toc` is the cheap form of that question: a stub bundle fails it with
  // "Document Missing Template Error" in well under a second.
  bool salvageable = false;
  if (!outcome.refusal.empty()) {
    proc::Options probe_opts;
    probe_opts.timeout = std::chrono::milliseconds(20000);
    probe_opts.cancel = config.cancel;
    const BundleReadability readable =
        bundle_is_readable(bundle_path, probe_opts);
    salvageable = readable.readable;
    if (salvageable) {
      out.partial = true;
      out.partial_reasons.push_back(
          "xctrace did not exit on its own (" + outcome.refusal +
          ") but the trace bundle it wrote is readable, so this capture is "
          "what it managed to record rather than nothing");
    }
  }
  if (!outcome.refusal.empty() && !salvageable) {
    result.error = outcome.refusal;
    return finish(model::CapabilityStatus::kUnsupported, outcome.refusal,
                  {"no readable trace was produced, so nothing about the app "
                   "was measured; this is a provider failure and not an "
                   "absence of findings",
                   std::filesystem::exists(bundle_path)
                       ? "a bundle exists at " + bundle_path +
                             " but `xctrace export --toc` could not read it, "
                             "which is what an unfinished recording looks like"
                       : "no bundle was written at all"});
  }
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
    std::vector<std::string> limits = {
        "the recording and the export both succeeded, so this is an empty or "
        "unreadable table rather than a missing provider"};
    // An empty table is the symptom. The commonest cause is not a damaged
    // trace: Instruments samples through the unified log store, and a process
    // without Full Disk Access cannot open it -- xctrace calls that "the log
    // archive is corrupt or incomplete", which sends people looking for
    // corruption that is not there. Asked only now, when something already
    // came back empty, so a working capture pays nothing.
    // Before blaming anything, check whether the samples are simply in a
    // table this build does not read. Saying "no samples" while they sit in
    // the bundle is the same mistake as refusing a recording that xctrace
    // finished but did not exit from.
    proc::Options alt_opts;
    alt_opts.timeout = std::chrono::milliseconds(20000);
    alt_opts.cancel = config.cancel;
    const AlternateSampleTable alt =
        find_unread_sample_table(bundle_path, alt_opts);
    if (alt.found()) {
      limits.push_back(
          "the samples are not missing: this bundle holds " +
          std::to_string(alt.rows) + " row(s) in the `" + alt.schema +
          "` table, which this build does not parse -- it reads "
          "`time-profile` only. That is a gap in this tool, not an empty "
          "capture, and the recording at " + bundle_path +
          " can be opened in Instruments.");
    }

    const LogStoreAccess log_access = probe_log_store_access();
    if (!log_access.readable) {
      limits.push_back(
          "this process cannot read the unified log store, which is what "
          "Instruments samples through: `log show` said \"" +
          log_access.detail +
          "\". That is the likely cause of the empty table, and it is a "
          "permission rather than a damaged machine -- grant Full Disk Access "
          "to whatever runs this tool (System Settings > Privacy & Security > "
          "Full Disk Access), or run the capture from Xcode, which already has "
          "it.");
      // Permission-denied rather than limited: the distinction decides
      // whether someone goes looking for a broken trace or a checkbox.
      return finish(model::CapabilityStatus::kPermissionDenied, result.error,
                    std::move(limits));
    }
    return finish(model::CapabilityStatus::kLimited, result.error,
                  std::move(limits));
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
