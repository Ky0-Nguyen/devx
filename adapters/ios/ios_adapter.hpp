// iOS adapter over Apple's public developer tooling on macOS.
//
// Every command and output schema used here was verified against the
// installed toolchain during M0 (Xcode 26.6, devicectl 518.33, xctrace 16.0);
// see docs/capabilities/ios-toolchain-probe.md for the recorded probes.
//
// Three findings shape this design, and none of them are assumptions:
//
//  1. `devicectl` states in its own help that "JSON output to a
//     user-provided file on disk is the ONLY supported interface for
//     scripts/programs to consume command output". We therefore always pass
//     --json-output and read the file; stdout is never parsed.
//
//  2. On a physical device, app and process enumeration require the
//     Developer Disk Image services. `deviceProperties.ddiServicesAvailable`
//     reports whether they are present, and
//     `connectionProperties.tunnelState` reports whether the device is
//     reachable at all. Both are read and surfaced rather than assumed.
//
//  3. `simctl listapps` emits an old-style NeXTSTEP property list, *not*
//     JSON. It is converted with `plutil` before parsing. A simulator is
//     never equated with a physical device (spec 4.1, J18).
//
// iOS gives no general foreground/suspended signal, so those states are
// reported as unknown rather than guessed (spec A09, 3.4).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/discovery/provider.hpp"

namespace mpi::ios {

// Parses the `result.devices[]` array of `devicectl list devices
// --json-output`.
std::vector<model::DeviceRef> parse_devicectl_devices(const json::Value& root,
                                                      const std::string& provider_version);

// Reads ddiServicesAvailable / developerModeStatus for one device, which gate
// deep profiling independently of whether the device is merely visible.
struct DeviceReadiness {
  std::optional<bool> ddi_services_available;
  std::string developer_mode_status;
  std::string tunnel_state;
  std::string pairing_state;
};
std::optional<DeviceReadiness> parse_devicectl_readiness(const json::Value& root,
                                                         const std::string& identifier);

// Parses `devicectl device info apps --json-output`.
struct InstalledApp {
  std::string bundle_id;
  std::string name;
  std::string version;
  // "User" / "System" / "Internal" as reported; an App Store app cannot
  // generally be deeply profiled and that is stated, not silently assumed.
  std::string app_type;
  std::optional<bool> profileable_hint;
};
std::vector<InstalledApp> parse_devicectl_apps(const json::Value& root);

// Parses `devicectl device info processes --json-output`.
struct DeviceProcess {
  std::int32_t pid = 0;
  std::string executable_path;
};
std::vector<DeviceProcess> parse_devicectl_processes(const json::Value& root);

// Parses `simctl list devices --json`.
std::vector<model::DeviceRef> parse_simctl_devices(const json::Value& root,
                                                   const std::string& provider_version);

// Parses the JSON form of `simctl listapps` (after plutil conversion).
std::vector<InstalledApp> parse_simctl_listapps(const json::Value& root);

// Parses `simctl spawn <udid> launchctl list`. App processes appear as
// "UIKitApplication:<bundle-id>[hash][...]" -- launchd itself attributes the
// process to the bundle id, which is provider-attributed ownership evidence.
struct LaunchdEntry {
  std::optional<std::int32_t> pid;  // absent for a loaded-but-not-running job
  std::string label;
  std::string bundle_id;  // set only for UIKitApplication labels
};
std::vector<LaunchdEntry> parse_launchctl_list(const std::string& text);

class IosAdapter final : public discovery::Provider {
 public:
  IosAdapter();

  model::Platform platform() const override { return model::Platform::kIos; }
  std::string name() const override { return "apple-devicetools"; }

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

  // Include simulators in device listings. Simulator results are always
  // tagged DeviceForm::kSimulator and never merged with physical results.
  void set_include_simulators(bool v) { include_simulators_ = v; }

  std::string devicectl_version(const discovery::ProviderOptions& opts) const;
  std::string xctrace_version(const discovery::ProviderOptions& opts) const;

 private:
  bool include_simulators_ = true;
  // Runs an `xcrun devicectl ...` command with --json-output into a temporary
  // file and parses it. Returns nullopt on any failure, with `error` set.
  std::optional<json::Value> run_devicectl_json(
      const std::vector<std::string>& args, const discovery::ProviderOptions& opts,
      std::string* error) const;
};

}  // namespace mpi::ios
