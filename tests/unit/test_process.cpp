#include <sstream>

#include "core/util/cancel.hpp"
#include "core/util/process.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

MPI_TEST(process_runs_and_captures_output, {"J05"}) {
  const auto r = proc::run({"/bin/echo", "hello world"});
  MPI_CHECK(r.spawned);
  MPI_CHECK_EQ(r.exit_code, 0);
  MPI_CHECK(r.out.find("hello world") != std::string::npos);
}

MPI_TEST(process_argv_is_never_shell_interpreted, {"J05"}) {
  // If this were passed through a shell, the semicolon would start a second
  // command and `id` would run. As a literal argv element it cannot.
  const auto r = proc::run({"/bin/echo", "; id", "&& whoami", "$(pwd)", "`ls`"});
  MPI_CHECK(r.ok());
  MPI_CHECK(r.out.find("; id") != std::string::npos);
  MPI_CHECK(r.out.find("&& whoami") != std::string::npos);
  MPI_CHECK(r.out.find("$(pwd)") != std::string::npos);
  // No command substitution happened: the backticks are still there.
  MPI_CHECK(r.out.find("`ls`") != std::string::npos);
}

MPI_TEST(process_reports_missing_executable_distinctly, {"A06"}) {
  const auto r = proc::run({"/nonexistent/definitely-not-a-real-binary"});
  // A missing tool is not an exit status: it is a spawn failure, which the
  // capability contract reports as `unsupported` with a recovery action.
  MPI_CHECK(!r.spawned);
  MPI_CHECK(!r.spawn_error.empty());
  MPI_CHECK(!r.ok());
}

MPI_TEST(process_nonzero_exit_is_not_an_error, {"A12"}) {
  const auto r = proc::run({"/bin/sh", "-c", "exit 3"});
  MPI_CHECK(r.spawned);
  MPI_CHECK_EQ(r.exit_code, 3);
  MPI_CHECK(!r.ok());
  // Spawned-but-failed is distinguishable from never-started.
  MPI_CHECK(r.spawn_error.empty());
}

MPI_TEST(process_times_out_and_says_so, {"D17"}) {
  proc::Options o;
  o.timeout = std::chrono::milliseconds(300);
  const auto r = proc::run({"/bin/sleep", "10"}, o);
  MPI_CHECK(r.spawned);
  MPI_CHECK(r.timed_out);
  MPI_CHECK(!r.ok());
  MPI_CHECK_MSG(r.duration < std::chrono::milliseconds(5000),
                "the timeout must actually cut the wait short");
}

MPI_TEST(process_honours_cancellation, {"D04", "J11"}) {
  CancellationSource src;
  src.cancel();  // already cancelled before the call
  proc::Options o;
  o.cancel = src.token();
  o.timeout = std::chrono::milliseconds(10000);
  const auto r = proc::run({"/bin/sleep", "10"}, o);
  MPI_CHECK(r.cancelled);
  MPI_CHECK(!r.ok());
  MPI_CHECK(r.duration < std::chrono::milliseconds(5000));
}

MPI_TEST(process_bounds_output_size, {"J03"}) {
  proc::Options o;
  o.max_output_bytes = 4096;
  o.timeout = std::chrono::milliseconds(10000);
  // yes(1) produces unbounded output; the runner must cap and terminate it.
  const auto r = proc::run({"/bin/sh", "-c", "yes abcdefgh"}, o);
  MPI_CHECK(r.spawned);
  MPI_CHECK_MSG(r.out.size() <= 4096 + 16384,
                "output must be bounded, got " + std::to_string(r.out.size()));
  MPI_CHECK(r.err.find("max_output_bytes") != std::string::npos);
}

MPI_TEST(process_rejects_nul_in_argument, {"J05"}) {
  std::string bad("abc");
  bad.push_back('\0');
  bad += "def";
  MPI_CHECK(!proc::is_safe_argument(bad, false));
  const auto r = proc::run({"/bin/echo", bad});
  MPI_CHECK(!r.spawned);
}

MPI_TEST(process_rejects_option_like_identifier, {"A20", "J05"}) {
  // An app identifier must never be able to masquerade as a tool flag.
  MPI_CHECK(!proc::is_safe_argument("--help", true));
  MPI_CHECK(!proc::is_safe_argument("-rf", true));
  MPI_CHECK(proc::is_safe_argument("com.example.app", true));
  MPI_CHECK(proc::is_safe_argument("--help", false));
}

MPI_TEST(process_which_resolves_and_reports_absence, {"A06"}) {
  MPI_CHECK(proc::which("sh").has_value());
  MPI_CHECK(!proc::which("mpi-definitely-not-installed-xyz").has_value());
  MPI_CHECK(proc::which("/bin/sh").has_value());
  MPI_CHECK(!proc::which("/bin/definitely-not-here").has_value());
}

MPI_TEST(process_separates_stdout_and_stderr, {"F11"}) {
  const auto r = proc::run({"/bin/sh", "-c", "echo to_out; echo to_err 1>&2"});
  MPI_CHECK(r.ok());
  MPI_CHECK(r.out.find("to_out") != std::string::npos);
  MPI_CHECK(r.out.find("to_err") == std::string::npos);
  MPI_CHECK(r.err.find("to_err") != std::string::npos);
}

MPI_TEST(process_env_override_applies, {"C19"}) {
  proc::Options o;
  o.env_overrides.push_back("MPI_TEST_VAR=sentinel-value");
  const auto r = proc::run({"/bin/sh", "-c", "echo $MPI_TEST_VAR"}, o);
  MPI_CHECK(r.ok());
  MPI_CHECK(r.out.find("sentinel-value") != std::string::npos);
  // PATH is inherited, so the child can still find its tools.
  const auto r2 = proc::run({"/bin/sh", "-c", "echo ${PATH:+haspath}"}, o);
  MPI_CHECK(r2.out.find("haspath") != std::string::npos);
}
