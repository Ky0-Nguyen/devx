// `mpi sdk-network` -- Inspect a build that runs no inspector.
//
// A release build has no debugger for `mpi inspect` to attach to. The in-app
// SDK's `trackNetwork()` sends the app's own fetch/XMLHttpRequest traffic to
// this listener instead, and what it prints at the end is the same report
// `mpi inspect --detail` prints.
//
// The first stdout line is machine-readable, as `sdk-bridge`'s is, so a test
// or a script reads the endpoint and token without parsing prose.
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "apps/cli/cli.hpp"
#include "core/sdk/network_inspect.hpp"
#include "core/util/json.hpp"

namespace mpi::cli {

ExitCode cmd_sdk_network(const Invocation& inv) {
  std::uint16_t port = 0;
  if (inv.has_flag("port")) {
    const int p = std::atoi(inv.flag("port").c_str());
    if (p < 1 || p > 65535) {
      std::cerr << "error: --port must be between 1 and 65535\n";
      return ExitCode::kUsage;
    }
    port = static_cast<std::uint16_t>(p);
  }

  sdk::NetworkInspect listener;
  std::string error;
  if (!listener.start(port, inv.flag("token"), &error)) {
    std::cerr << "error: cannot start the SDK listener: " << error << "\n";
    return ExitCode::kCollectionError;
  }

  std::cout << "endpoint http://127.0.0.1:" << listener.port() << " token "
            << listener.token() << std::endl;
  std::cerr << "listening on 127.0.0.1:" << listener.port()
            << " (loopback only, token required)\n"
            << "  in the app:  trackNetwork({ endpoint: 'http://127.0.0.1:"
            << listener.port() << "', token: '" << listener.token() << "' })\n"
            << "  for an Android device: " << listener.adb_reverse_command() << "\n"
            << "  for the iOS simulator: loopback is already shared\n";

  const int seconds = inv.has_flag("duration-s")
                          ? std::atoi(inv.flag("duration-s").c_str())
                          : 0;
  if (seconds > 0) {
    std::cerr << "  stopping after " << seconds << "s\n";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      if (inv.global.cancel.cancelled()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } else {
    std::cerr << "  reading until stdin closes (Ctrl-D), or Ctrl-C\n";
    std::string line;
    while (!inv.global.cancel.cancelled() && std::getline(std::cin, line)) {
      if (line == "stop") break;
    }
  }

  listener.stop();
  const std::string text = listener.snapshot().to_json().dump(2);

  const std::string out_path = inv.flag("out");
  if (!out_path.empty()) {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot write " << out_path << "\n";
      return ExitCode::kCollectionError;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    std::cerr << "wrote " << out_path
              << " (it holds headers and bodies verbatim, tokens included)\n";
  }
  std::cout << text << std::endl;

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  if (!listener.hello().has_value()) {
    std::cerr << "note: no app connected, so nothing was received. Check the "
                 "endpoint, the token, and `adb reverse` on Android.\n";
    return ExitCode::kInconclusive;
  }
  return ExitCode::kOk;
}

}  // namespace mpi::cli
