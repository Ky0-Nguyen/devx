// Child-process execution with an explicit argv vector.
//
// Spec section 5 forbids shell-string interpolation using app names,
// identifiers, trace fields, or source paths, and spec J05 requires
// command/argument injection to be rejected. There is therefore no API here
// that accepts a command line string: callers must supply argv elements, which
// are passed to posix_spawnp without a shell.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"

namespace mpi::proc {

struct Result {
  // Set when the child could not be started at all (missing tool, EACCES).
  bool spawned = false;
  int exit_code = -1;
  bool timed_out = false;
  bool cancelled = false;
  std::string out;
  std::string err;
  std::chrono::milliseconds duration{0};
  // Populated when spawned == false.
  std::string spawn_error;

  bool ok() const { return spawned && exit_code == 0 && !timed_out && !cancelled; }
};

struct Options {
  std::chrono::milliseconds timeout{15000};
  // Refuse to buffer more than this from either stream. Spec section 15
  // requires bounded file/event sizes and parser memory.
  std::size_t max_output_bytes = 64ull * 1024ull * 1024ull;
  CancellationToken cancel;
  // Extra environment entries, each "KEY=VALUE". The parent environment is
  // inherited; entries here override.
  std::vector<std::string> env_overrides;
};

// Validates that `arg` contains no NUL and no leading '-' when
// `reject_option_like` is set. Used to keep an app identifier or file path
// from being reinterpreted as a flag by the invoked tool.
bool is_safe_argument(const std::string& arg, bool reject_option_like);

// Runs argv[0] via PATH lookup (posix_spawnp), no shell involved.
Result run(const std::vector<std::string>& argv, const Options& opts);
inline Result run(const std::vector<std::string>& argv) {
  return run(argv, Options{});
}

// Resolves an executable on PATH. Returns nullopt when absent, which the
// capability contract reports as `unsupported` with a recovery action rather
// than as a failure.
std::optional<std::string> which(const std::string& exe);

}  // namespace mpi::proc
