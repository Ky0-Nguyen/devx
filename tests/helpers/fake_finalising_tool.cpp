// A stand-in for `xctrace record`, for testing how this tool stops a child.
//
// It reproduces the two behaviours that made the real thing lose data:
//
//   * it **ignores SIGTERM**, so the default stop signal cannot end it;
//   * it writes its output file only on **SIGINT**, and takes a moment over
//     it, exactly as Instruments finalises a trace bundle.
//
// Without this, the fix for that data loss was verified only by the absence
// of new test failures -- which is not verification.
//
// Usage: fake_finalising_tool <output-path> [write-delay-ms]
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;

static void on_int(int sig) {
  (void)sig;
  interrupted = 1;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const char* out = argv[1];
  const long delay_ms = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 300;

  // Ignore SIGTERM outright: the point of the test is that a polite SIGTERM
  // is not enough for a tool like this.
  std::signal(SIGTERM, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_int;
  sigaction(SIGINT, &sa, nullptr);

  // The line the real tool prints once it is actually recording.
  std::printf("Ctrl-C to stop the recording\n");
  std::fflush(stdout);

  while (!interrupted) {
    struct timespec ts = {0, 20L * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }

  // Finalising takes time, which is the whole reason a grace period is
  // needed rather than an immediate SIGKILL.
  struct timespec ts = {delay_ms / 1000, (delay_ms % 1000) * 1000L * 1000};
  nanosleep(&ts, nullptr);
  std::FILE* f = std::fopen(out, "wb");
  if (f == nullptr) return 1;
  std::fputs("finalised", f);
  std::fclose(f);
  return 0;
}
