// A minimal test harness.
//
// Written rather than vendored so the third-party license inventory the
// specification asks for stays empty and the build has no network dependency.
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace mpi::test {

struct Case {
  std::string name;
  std::function<void()> fn;
  // Checklist ids from specification section 18 that this case covers.
  std::vector<std::string> requirement_ids;
};

// Registers a test case. Returns a dummy int so it can run at static init.
int register_case(const char* name, std::function<void()> fn,
                  std::vector<std::string> requirement_ids);

void fail(const std::string& expr, const char* file, int line,
          const std::string& detail);

int run_all(int argc, char** argv);

}  // namespace mpi::test

// TEST(name, {req ids}) { ... }
#define MPI_TEST(name, ...)                                                  \
  static void mpi_test_##name();                                             \
  static const int mpi_test_reg_##name =                                     \
      ::mpi::test::register_case(#name, mpi_test_##name, __VA_ARGS__);       \
  static void mpi_test_##name()

#define MPI_CHECK(cond)                                                      \
  do {                                                                       \
    if (!(cond)) ::mpi::test::fail(#cond, __FILE__, __LINE__, "");           \
  } while (false)

#define MPI_CHECK_MSG(cond, msg)                                             \
  do {                                                                       \
    if (!(cond)) ::mpi::test::fail(#cond, __FILE__, __LINE__, (msg));        \
  } while (false)

#define MPI_CHECK_EQ(a, b)                                                   \
  do {                                                                       \
    auto mpi_a = (a);                                                        \
    auto mpi_b = (b);                                                        \
    if (!(mpi_a == mpi_b)) {                                                 \
      std::ostringstream mpi_os;                                             \
      mpi_os << "expected equal:\n      left:  " << mpi_a                    \
             << "\n      right: " << mpi_b;                                  \
      ::mpi::test::fail(#a " == " #b, __FILE__, __LINE__, mpi_os.str());     \
    }                                                                        \
  } while (false)

#define MPI_CHECK_NEAR(a, b, eps)                                            \
  do {                                                                       \
    const double mpi_a = static_cast<double>(a);                             \
    const double mpi_b = static_cast<double>(b);                             \
    if (std::fabs(mpi_a - mpi_b) > (eps)) {                                  \
      std::ostringstream mpi_os;                                             \
      mpi_os << "expected " << mpi_a << " within " << (eps) << " of " << mpi_b; \
      ::mpi::test::fail(#a " ~= " #b, __FILE__, __LINE__, mpi_os.str());     \
    }                                                                        \
  } while (false)
