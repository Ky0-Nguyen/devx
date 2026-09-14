#pragma once

#include <cstdint>
#include <string>

namespace mpi::time_util {

// ISO-8601 UTC instant, second precision, e.g. "2026-09-14T08:13:44Z".
// Used only for provenance stamps -- never for durations.
std::string now_iso8601_utc();

// Host monotonic nanoseconds. Durations are always computed from a monotonic
// source (spec section 6); a wall-clock or timezone change must not alter a
// measured duration (spec D14).
std::int64_t monotonic_ns();

// Renders a nanosecond duration for human output, e.g. "12.480 ms".
std::string format_duration_ns(std::int64_t ns);

}  // namespace mpi::time_util
