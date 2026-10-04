#include "adapters/android/view_dump.hpp"

#include "core/util/process.hpp"

namespace mpi::android {

ViewDump dump_activity_top(const std::string& adb_path,
                           const std::string& serial,
                           std::chrono::milliseconds timeout,
                           const CancellationToken& cancel) {
  ViewDump res;
  if (!proc::is_safe_argument(serial, true)) {
    res.error = "refusing a serial that would be read as an option";
    return res;
  }
  proc::Options po;
  po.timeout = timeout;
  po.cancel = cancel;
  const auto r =
      proc::run({adb_path, "-s", serial, "shell", "dumpsys", "activity", "top"}, po);
  if (!r.ok()) {
    res.error = r.spawned ? (r.timed_out ? "dumpsys activity top did not finish in time"
                                         : r.err)
                          : r.spawn_error;
    return res;
  }
  res.ok = true;
  res.text = r.out;
  return res;
}

}  // namespace mpi::android
