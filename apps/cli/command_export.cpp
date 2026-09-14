#include <fstream>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/session/session_store.hpp"

namespace mpi::cli {

ExitCode cmd_export(const Invocation& inv) {
  if (inv.positional.empty()) {
    std::cerr << "error: export needs a session package directory\n";
    return ExitCode::kUsage;
  }
  const std::string dir = inv.positional.front();
  const auto loaded = session::load_package(dir);
  if (!loaded.ok) {
    std::cerr << "error: " << loaded.error << "\n";
    return ExitCode::kCollectionError;
  }

  for (const auto& f : loaded.checksum_failures) {
    warn("checksum: " + f);
  }

  const std::string format = inv.flag("format", "markdown");
  std::string source;
  if (format == "markdown" || format == "md") {
    source = dir + "/report.md";
  } else if (format == "json") {
    source = dir + "/report.json";
  } else {
    std::cerr << "error: --format must be json or markdown\n";
    return ExitCode::kUsage;
  }

  std::ifstream in(source, std::ios::binary);
  if (!in) {
    std::cerr << "error: " << source
              << " is not present in this session package\n";
    return ExitCode::kNotFound;
  }
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

  const std::string out_path = inv.flag("out");
  if (out_path.empty()) {
    std::cout << content;
  } else {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot write " << out_path << "\n";
      return ExitCode::kCollectionError;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    std::cerr << "wrote " << out_path << "\n";
  }

  // A package whose checksums do not verify is reported as a collection
  // problem even though the bytes were readable.
  if (!loaded.checksum_failures.empty()) return ExitCode::kCollectionError;
  return ExitCode::kOk;
}

}  // namespace mpi::cli
