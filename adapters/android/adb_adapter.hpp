// Android adapter over Android Platform Tools (adb).
//
// Everything here is built on commands whose output was verified against the
// installed platform-tools version; where output shape varies by Android
// release, the parser is tested against recorded fixtures rather than assumed
// (spec section 3.3).
//
// Two facts the design turns on:
//   * Installed-package metadata and live process observations are *distinct
//     inputs*, reconciled against each other. Neither alone establishes that
//     a process belongs to the app.
//   * A shared UID or a process-name prefix is not sufficient ownership
//     evidence. Such processes are listed and left unassigned (spec B06, B07).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/discovery/provider.hpp"

namespace mpi::android {

// One row of `adb shell ps -A -o PID,PPID,USER,NAME` style output.
struct PsRow {
  std::int32_t pid = 0;
  std::int32_t ppid = 0;
  std::string user;   // "u0_a123" or a numeric uid
  std::string name;   // process name, e.g. "com.example.app:remote"
  std::optional<std::int32_t> uid;
  std::optional<int> android_user_id;
};

// Parses `ps` output. Tolerates column reordering by reading the header, and
// returns nothing rather than guessing when the header is unrecognizable.
std::vector<PsRow> parse_ps_output(const std::string& text,
                                   std::vector<std::string>& warnings);

// Parses `adb devices -l`.
std::vector<model::DeviceRef> parse_adb_devices(const std::string& text,
                                                const std::string& adb_version);

// Parses `pm list packages -U --user N` ("package:NAME uid:UID").
struct PackageRow {
  std::string package;
  std::optional<std::int32_t> uid;
};
std::vector<PackageRow> parse_pm_list_packages(const std::string& text);

// Extracts the process start time from `/proc/<pid>/stat` field 22 (starttime
// in clock ticks since boot). Returns nullopt when the line is malformed --
// without it, PID reuse cannot be ruled out, and the caller downgrades
// ownership accordingly (spec B02).
std::optional<std::string> parse_proc_stat_starttime(const std::string& stat_line);

// Reads "profileable" / "debuggable" from `dumpsys package` output.
struct PackageFlags {
  std::optional<bool> debuggable;
  std::optional<bool> profileable_shell;
  std::string version_name;
  std::optional<std::int64_t> version_code;
};
PackageFlags parse_dumpsys_package_flags(const std::string& text);

class AdbAdapter final : public discovery::Provider {
 public:
  explicit AdbAdapter(std::string adb_path = "adb");

  model::Platform platform() const override { return model::Platform::kAndroid; }
  std::string name() const override { return "adb"; }

  void probe(model::CapabilityMatrix& out,
             const discovery::ProviderOptions& opts) const override;
  std::vector<model::DeviceRef> list_devices(
      const discovery::ProviderOptions& opts,
      std::vector<std::string>& errors) const override;
  std::vector<model::AppEntry> list_apps(
      const model::DeviceRef& device, const discovery::ProviderOptions& opts,
      std::vector<std::string>& errors,
      bool& enumeration_failed) const override;
  std::vector<model::ProcessInstance> resolve_processes(
      const model::DeviceRef& device, const model::ApplicationKey& app,
      const discovery::ProviderOptions& opts,
      std::vector<std::string>& errors) const override;

  // Reported version string, or empty when adb is absent.
  std::string adb_version(const discovery::ProviderOptions& opts) const;

 private:
  std::string adb_path_;
  // Builds an argv for "adb -s SERIAL shell ..." with each shell word passed
  // as its own argv element. No string interpolation of identifiers is ever
  // performed (spec section 5, J05).
  std::vector<std::string> shell_argv(const std::string& serial,
                                      const std::vector<std::string>& args) const;
};

}  // namespace mpi::android
