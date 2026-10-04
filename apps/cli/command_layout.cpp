// `mpi layout` -- how the screen an app is showing is built: views per
// screen, how deep they nest, how many are hidden or off screen, and which
// navigation stacks hold how many screens.
//
// Selected by package name or bundle id, like everything else, and nothing is
// added to the app:
//
//   - Android (emulator or device): `dumpsys activity top`. Nothing runs
//     inside the app.
//   - iOS simulator: the layout probe, injected when the app is launched with
//     `--relaunch`. The app is restarted to do it, so that is never implicit;
//     a later run without the flag reconnects to the probe still loaded.
//   - A physical iOS device: refused. Code signing will not load an injected
//     library there, and there is no other route to its view tree.
//
// The rules live in adapters/layout so DevX applies the same ones.
#include <iostream>

#include "adapters/layout/layout_capture.hpp"
#include "apps/cli/cli.hpp"

namespace mpi::cli {

ExitCode cmd_layout(const Invocation& inv) {
  if (inv.global.app.empty()) {
    std::cerr << "error: layout needs --app <package-name|bundle-id>\n";
    return ExitCode::kUsage;
  }
  layout::CaptureOptions opts;
  opts.relaunch = inv.has_flag("relaunch");
  opts.timeout = std::chrono::milliseconds(inv.global.timeout_ms);
  opts.cancel = inv.global.cancel;
  if (inv.has_flag("wait-s")) {
    const int secs = std::atoi(inv.flag("wait-s").c_str());
    if (secs < 0 || (secs == 0 && inv.flag("wait-s") != "0")) {
      std::cerr << "error: --wait-s must be zero or a positive integer\n";
      return ExitCode::kUsage;
    }
    opts.settle = std::chrono::milliseconds(secs * 1000);
  }
  if (!inv.global.quiet) {
    opts.progress = [](const std::string& m) { std::cerr << m << "\n"; };
  }
  // Kept on disk by default, so `mpi mcp` can hand the whole tree to a model.
  if (!inv.has_flag("no-save")) opts.save_to = inv.global.sessions_dir;

  auto svc = make_discovery(inv.global);
  const auto snap = svc.snapshot(provider_options(inv.global), /*include_apps=*/false);
  model::DeviceRef device;
  ExitCode code = ExitCode::kOk;
  if (!resolve_device(inv, snap, device, code)) return code;

  const auto capture = layout::capture_layout(device, inv.global.app, opts);
  if (!inv.global.quiet) {
    for (const auto& n : capture.notes) std::cerr << "note: " << n << "\n";
  }
  if (!capture.ok()) {
    std::cerr << "error: " << capture.error << "\n";
    if (capture.failure == layout::Failure::kNotLoaded) {
      std::cerr << "  run again with --relaunch to load it\n";
    }
    switch (capture.failure) {
      case layout::Failure::kUnsupported:
      case layout::Failure::kProbeMissing: return ExitCode::kUnsupportedOperation;
      case layout::Failure::kNotOnScreen:  return ExitCode::kNotFound;
      case layout::Failure::kCancelled:    return ExitCode::kCancelled;
      case layout::Failure::kNotLoaded:
      case layout::Failure::kCollection:
      case layout::Failure::kNone:         return ExitCode::kCollectionError;
    }
    return ExitCode::kCollectionError;
  }
  const bool tree = inv.has_flag("tree");
  if (inv.global.json) {
    json::Value out = capture.report->to_json(tree);
    if (!capture.saved_id.empty()) {
      out.set("saved_observation", json::Value::string(capture.saved_id));
    }
    print_json(out);
  } else {
    std::cout << capture.report->to_text(tree);
  }
  if (!capture.saved_id.empty() && !inv.global.quiet) {
    std::cerr << "saved: " << capture.saved_path
              << "\n  read it back with `mpi mcp` (read_observation " << capture.saved_id
              << "); --no-save skips this\n";
  }
  if (!capture.save_error.empty()) warn("not saved: " + capture.save_error);
  return ExitCode::kOk;
}

}  // namespace mpi::cli
