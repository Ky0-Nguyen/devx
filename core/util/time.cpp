#include "core/util/time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace mpi::time_util {

std::string now_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return buf;
}

std::int64_t monotonic_ns() {
  const auto d = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

std::string format_duration_ns(std::int64_t ns) {
  char buf[48];
  if (ns < 0) {
    std::snprintf(buf, sizeof(buf), "-%s",
                  format_duration_ns(-ns).c_str());
    return buf;
  }
  if (ns < 1000) {
    std::snprintf(buf, sizeof(buf), "%lld ns", static_cast<long long>(ns));
  } else if (ns < 1000000) {
    std::snprintf(buf, sizeof(buf), "%.3f us", static_cast<double>(ns) / 1e3);
  } else if (ns < 1000000000LL) {
    std::snprintf(buf, sizeof(buf), "%.3f ms", static_cast<double>(ns) / 1e6);
  } else {
    std::snprintf(buf, sizeof(buf), "%.3f s", static_cast<double>(ns) / 1e9);
  }
  return buf;
}

}  // namespace mpi::time_util
