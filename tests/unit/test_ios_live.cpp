// Live capture for an iOS simulator, and the unit trap it rests on.
//
// The task-level CPU counters are in mach absolute time units; the per-thread
// counters are in nanoseconds. Two units in one API family, and reading the
// first as the second understates CPU by the timebase -- 41.67x on Apple
// Silicon. Measured against a process burning a known 3.0 s: 0.072 s read as
// nanoseconds, 2.999 s with the timebase applied, 3.00 s from `ps`.
//
// A CPU series fifty times too small is worse than no CPU series, which is
// why this collector did not exist until the discrepancy was explained.
#include <mach/mach_time.h>
#include <unistd.h>

#include "adapters/ios/simulator_host_collector.hpp"
#include "tests/unit/test_framework.hpp"

MPI_TEST(mach_ticks_convert_by_the_hosts_own_timebase, {}) {
  mach_timebase_info_data_t tb{};
  MPI_CHECK(mach_timebase_info(&tb) == KERN_SUCCESS);
  MPI_CHECK(tb.denom != 0);

  // Checked against the timebase rather than a hard-coded 41.6667: this must
  // stay correct on an Intel Mac, where the ratio is 1/1 and reading the
  // counters as nanoseconds happens to work -- which is exactly why the bug
  // hid for so long.
  const std::uint64_t ticks = 1'000'000;
  const std::int64_t expected = static_cast<std::int64_t>(
      static_cast<double>(ticks) * tb.numer / tb.denom);
  const std::int64_t got = mpi::ios::mach_ticks_to_ns(ticks);
  // Integer arithmetic, so allow one unit of rounding per conversion step.
  MPI_CHECK_MSG(got >= expected - 2 && got <= expected + 2,
                "a tick count converts to the timebase's nanoseconds");

  MPI_CHECK_MSG(mpi::ios::mach_ticks_to_ns(0) == 0, "zero is zero");

  // On Apple Silicon this is the factor that was missing.
  if (tb.numer == 125 && tb.denom == 3) {
    MPI_CHECK_MSG(mpi::ios::mach_ticks_to_ns(72'000'000) > 2'900'000'000LL &&
                      mpi::ios::mach_ticks_to_ns(72'000'000) < 3'100'000'000LL,
                  "the reading that looked like 0.072s is really ~3.0s");
  }
}

MPI_TEST(a_huge_tick_count_does_not_wrap, {}) {
  // A long-running process carries enough ticks that `ticks * numer`
  // overflows 64 bits if multiplied naively, and a wrapped CPU total reads as
  // the app going backwards in time.
  const std::uint64_t enormous = 1ULL << 60;
  const std::int64_t ns = mpi::ios::mach_ticks_to_ns(enormous);
  MPI_CHECK_MSG(ns > 0, "the conversion stays positive");
  // A year is about 3.15e16 ns; 2^60 ticks is far more, but it must not
  // come back small or negative.
  MPI_CHECK_MSG(ns > (1LL << 59), "and is not truncated to a small number");
}

MPI_TEST(reading_a_pid_that_is_not_there_is_a_lost_reading, {}) {
  // pid 1 is launchd: it exists and is not ours to read in detail, and
  // either way a failure must be an error rather than a measurement of zero.
  const auto gone = mpi::ios::sample_host_process(999999);
  MPI_CHECK_MSG(!gone.ok, "a missing process does not report ok");
  MPI_CHECK_MSG(!gone.error.empty(), "and says why");
  MPI_CHECK_MSG(gone.cpu_time_ns == 0 && gone.footprint_bytes == 0,
                "the fields stay zero, and `ok` is what distinguishes that "
                "from a genuine zero reading");

  MPI_CHECK_MSG(!mpi::ios::sample_host_process(0).ok, "pid 0 is refused");
  MPI_CHECK_MSG(!mpi::ios::sample_host_process(-1).ok,
                "and a negative pid is refused rather than cast");
  MPI_CHECK_MSG(mpi::ios::sample_host_threads(999999).empty(),
                "and there are no threads to list");
}

MPI_TEST(this_process_reads_its_own_cpu_and_memory, {}) {
  // The test binary is a host process, so it is a valid subject -- and one
  // whose numbers can be sanity-checked without a simulator.
  const auto self = mpi::ios::sample_host_process(getpid());
  MPI_CHECK_MSG(self.ok, "a live same-user process reads");
  MPI_CHECK_MSG(self.cpu_time_ns > 0, "it has used some CPU by now");
  // A test that has run for milliseconds cannot have used an hour of CPU.
  // This is the assertion that fails if the timebase is applied twice.
  MPI_CHECK_MSG(self.cpu_time_ns < 3'600'000'000'000LL,
                "and not an implausible amount, which is what a doubled "
                "timebase conversion would produce");
  MPI_CHECK_MSG(self.footprint_bytes > 0, "and it occupies memory");
  MPI_CHECK_MSG(self.thread_count >= 1, "with at least one thread");

  const auto threads = mpi::ios::sample_host_threads(getpid());
  MPI_CHECK_MSG(!threads.empty(), "its threads enumerate");
  // Per-thread times are already nanoseconds. If they were converted through
  // the timebase as well they would exceed the process total by ~42x.
  std::int64_t sum = 0;
  for (const auto& t : threads) sum += t.cpu_time_ns;
  MPI_CHECK_MSG(sum < self.cpu_time_ns * 40 + 1'000'000'000LL,
                "the per-thread sum is in the same unit as the process "
                "total, not the timebase multiple of it");
}

MPI_TEST(a_physical_device_is_refused_rather_than_degraded, {}) {
  mpi::model::DeviceRef phone;
  phone.platform = mpi::model::Platform::kIos;
  phone.form = mpi::model::DeviceForm::kPhysical;
  phone.device_id = "3FF46431-775C-59BB-AD26-D316DFAFA5A6";
  phone.hardware_udid = "00008101-000978CA11A1001E";

  mpi::model::ProcessInstance proc;
  proc.app.app_identifier = "com.example.app";
  mpi::session::CaptureConfig cfg;
  mpi::model::NormalizedTrace trace;
  mpi::ios::SimulatorHostCollector c;

  const auto r = c.begin(phone, {proc}, cfg, trace);
  MPI_CHECK_MSG(!r.started, "a physical device is refused");
  MPI_CHECK_MSG(r.error.find("simulator") != std::string::npos,
                "and the reason names the requirement");
  MPI_CHECK_MSG(r.error.find("host process") != std::string::npos,
                "specifically that the app must be a host process, which is "
                "what a simulator gives and a device does not");
  MPI_CHECK_MSG(trace.counters.empty(),
                "and nothing was recorded, rather than an empty series that "
                "would read as a capture with no activity");
}

MPI_TEST(a_batch_capture_here_is_refused_not_silently_downgraded, {}) {
  mpi::model::DeviceRef sim;
  sim.platform = mpi::model::Platform::kIos;
  sim.form = mpi::model::DeviceForm::kSimulator;
  mpi::session::CaptureConfig cfg;
  mpi::model::NormalizedTrace trace;
  mpi::ios::SimulatorHostCollector c;

  MPI_CHECK_MSG(c.supports_streaming(),
                "the collector streams, which is its whole purpose");
  const auto r = c.capture(sim, {}, cfg, trace);
  MPI_CHECK_MSG(!r.started, "a batch capture is refused");
  MPI_CHECK_MSG(r.error.find("xctrace") != std::string::npos,
                "and points at the collector that does give frames and "
                "stacks, so this is not a silent downgrade of `record`");
}

MPI_TEST(a_pid_lookup_distinguishes_not_running_from_not_there, {}) {
  // An unsafe argument must never reach a child process.
  const auto unsafe = mpi::ios::find_simulator_app_pid(
      "--not-a-udid", "com.example.app", std::chrono::milliseconds(2000));
  MPI_CHECK_MSG(!unsafe.found, "an option-shaped device id is refused");
  MPI_CHECK_MSG(unsafe.error.find("refusing") != std::string::npos,
                "and says so rather than running it");

  const auto bad_bundle = mpi::ios::find_simulator_app_pid(
      "00000000-0000-0000-0000-000000000000", "-rf",
      std::chrono::milliseconds(2000));
  MPI_CHECK(!bad_bundle.found);
}
