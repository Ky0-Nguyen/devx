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
#include "core/model/build.hpp"
#include "core/util/process.hpp"

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

// The process's own CPU time from /proc/<pid>/stat, in clock ticks.
//
// `utime` (field 14) and `stime` (field 15): time the process spent running
// in user mode and in the kernel on its own behalf. This is what separates a
// process that was *busy* from one that was *waiting* -- wall-clock time
// cannot tell those apart, and every CPU finding is read as though it could.
//
// Ticks, not nanoseconds, because the unit depends on the device's CLK_TCK
// and this parser does not guess it. The caller reads it and says what it
// read.
struct ProcCpuTime {
  std::int64_t utime_ticks = 0;
  std::int64_t stime_ticks = 0;
  std::int64_t total_ticks() const { return utime_ticks + stime_ticks; }
};
std::optional<ProcCpuTime> parse_proc_stat_cpu_time(const std::string& stat_line);

// Reads "profileable" / "debuggable" from `dumpsys package` output.
struct PackageFlags {
  std::optional<bool> debuggable;
  std::optional<bool> profileable_shell;
  std::string version_name;
  std::optional<std::int64_t> version_code;
  // What identifies the *build* under test, as opposed to the package name.
  //
  // Without these a session records which app it measured and nothing about
  // which build of it, so two captures of different builds are
  // indistinguishable -- and a reinstall between them is invisible. Spec B11
  // asks for a reinstall or update to revalidate build identity; that needs
  // an identity to revalidate.
  //
  // `code_path` changes on every install (Android randomises the directory
  // suffix), and `last_update_time` changes on every update, so either one
  // detects a build swap that left the version number alone -- which is
  // exactly what a developer iterating on one version does all day.
  std::string code_path;
  std::string first_install_time;
  std::string last_update_time;
  std::string signature_digest;
};
PackageFlags parse_dumpsys_package_flags(const std::string& text);

// Reads the app's build identity into `out`, from `dumpsys package`.
//
// Separate from discovery on purpose: discovery answers "which apps are
// there", and this answers "which build of this one am I about to measure" --
// the question a session has to record if two captures of the same package
// are ever to be compared. Spec B11 asks a reinstall or update to revalidate
// build identity, which needs an identity to revalidate; before this the
// Android build profile carried only the device's form and OS version, so a
// reinstall between two captures was invisible.
//
// Returns false with `error` set when the package could not be read. A
// missing fact is left absent rather than defaulted: an unknown version is
// not version zero.
bool read_app_build_facts(const std::string& adb_path,
                          const std::string& serial,
                          const std::string& package,
                          const proc::Options& opts,
                          model::BuildProfile& out, std::string& error);

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
