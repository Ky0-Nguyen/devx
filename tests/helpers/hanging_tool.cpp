// A tool that stops answering, used to pin what a wait loop does when the
// device tooling wedges.
//
// The bug this exists for: `mpi boot` checked its budget *before* each poll
// and then handed that poll its own fixed timeout, so a 180 s budget could
// run for 240 s. The case that produced it in the wild is a wedged
// `CoreSimulatorService`, where every `simctl list` hangs for the whole
// per-call timeout -- the budget bought three polls instead of ninety.
//
// Two modes, because the two failure shapes have different right answers,
// and both are selected through the environment rather than argv: this
// stands in for `adb`, which `boot()` invokes with a fixed argument list.
//
//   MPI_HANGING_TOOL_MARKER unset
//       Never answers. A wait loop must call this a tooling failure, not a
//       device that never came up.
//
//   MPI_HANGING_TOOL_MARKER=<path>
//       Answers once -- an empty but well-formed `adb devices` -- then
//       wedges. This is the shape that matters most: the baseline snapshot
//       succeeds, the boot is started, and only then does the tooling go
//       away, so there *is* something started to report on.
//
// The once-only answer goes through a file because each invocation is a
// fresh process; the caller supplies the path so two tests cannot collide.
//
// The hang is half a minute rather than indefinite: long enough to outlast
// any budget a test sets, short enough that a runaway run does not leave a
// process sitting around. This binary also stands in for the emulator, which
// is spawned detached and is *meant* to outlive the call.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main() {
  const char* marker = std::getenv("MPI_HANGING_TOOL_MARKER");
  if (marker != nullptr) {
    std::FILE* existing = std::fopen(marker, "r");
    if (existing == nullptr) {
      std::FILE* created = std::fopen(marker, "w");
      if (created != nullptr) std::fclose(created);
      std::printf("List of devices attached\n\n");
      return 0;
    }
    std::fclose(existing);
  }
  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}
