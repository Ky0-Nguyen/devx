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

#include <chrono>
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
  /// When CoreDevice last actually talked to this device, verbatim from
  /// `connectionProperties.lastConnectionDate`.
  ///
  /// This is here because `devicectl device info details` answers from a
  /// **cached record**: it returns `outcome: success` in a tenth of a second
  /// for a device that is not present at all, and every property it reports
  /// is then a description of the device as it was when last seen. Measured
  /// on a device that had been away four days -- it still reported
  /// `developerModeStatus: enabled`, which was a claim about the past
  /// presented as a fact about now.
  std::string last_connection_date;
};
std::optional<DeviceReadiness> parse_devicectl_readiness(const json::Value& root,
                                                         const std::string& identifier);

/// The refusal text for a device whose developer disk image services are not
/// mounted, qualified by when that was observed.
///
/// `devicectl device info details` answers from a cached record, so every
/// value it reports is an observation with a date attached. This is the
/// hardest refusal in the iOS path -- it stops app enumeration outright and
/// tells someone to go and do something in Xcode -- and it was stating the
/// value without saying when it was seen. The date was already parsed and
/// then never used, which is worse than not having it.
///
/// App enumeration only runs for a device discovery found usable, so in
/// practice the reading is fresh; this makes that checkable rather than
/// assumed.
std::string ddi_refusal_text(const DeviceReadiness& readiness);

/// Whether an executable path names an app's bundle.
///
/// The only attribution signal devicectl's process listing offers, and it is
/// weaker than it looks. The rule was `"/" + bundle_id + "/"` -- the bundle
/// id as a whole path segment -- and that matches nothing real. Checked
/// against an actual container path:
///
///     .../Bundle/Application/A303B644-.../io.pizzahut.hutbot.debug-1789449967523.app/HutBot
///
/// The bundle id is a segment *prefix* there, not a segment. On a physical
/// device it is absent entirely: the segment is `<Product>.app`. So the rule
/// could not match, every app reported `not_running` with no processes, and a
/// capture would tell someone to start an app that was already running.
///
/// This matches the bundle id at a segment boundary, allowing the `-<digits>`
/// and `.app` suffixes the installers add, and still refuses a different
/// bundle id that merely starts with the same text. A display name is never
/// used (spec 3.4).
bool executable_path_names_bundle(const std::string& executable_path,
                                  const std::string& bundle_id);

/// Turns a devicectl failure into a sentence about the device.
///
/// A device that is authorized at discovery and gone by the time apps are
/// enumerated is the commonest real iOS connection event -- someone unplugs
/// the phone -- and it produced Apple's raw text:
///
///     `devicectl device info apps` failed: ERROR: CoreDeviceService was
///     unable to locate a device matching the requested device identifier.
///     (DeviceIdentifier: ecid_2666084064886814)
///     (com.apple.dt.CoreDeviceError error 1011 (0x3F3))
///
/// The meaning of 1011 was already decoded for the reachability probe and not
/// applied here. The raw text is kept as well, because a tool's own words are
/// evidence and paraphrasing them away loses it.
std::string describe_devicectl_failure(const std::string& raw);

/// Whether a device answers *now*.
///
/// The passive listing cannot tell you this. `connectionProperties.tunnelState`
/// is a cached field, and `devicectl device info details` succeeds against a
/// device that is not there -- so both can describe a phone that left the desk
/// days ago. `devicectl device info lockState` cannot: it has to reach the
/// hardware, and it fails in about a tenth of a second when there is nothing
/// to reach.
///
/// It also answers a question a capture needs anyway: a locked device cannot
/// be driven.
enum class Reachability {
  kReachable,     // the device answered
  kNotFound,      // CoreDevice could not locate it
  kProbeFailed,   // the probe itself did not run (no devicectl, a timeout)
};
const char* to_string(Reachability r);

struct ReachabilityProbe {
  Reachability state = Reachability::kProbeFailed;
  /// The device's own answer, when it gave one.
  std::optional<bool> locked;
  std::string detail;
  /// What was run, so the reader knows what the answer rests on.
  std::string evidence;
  std::chrono::milliseconds took{0};
};

/// Probes one device. Costs a fraction of a second either way, which is why
/// it is worth doing rather than inferring.
///
/// `form` selects the right question. A physical device is asked through
/// `devicectl device info lockState`; a **simulator** is asked by running
/// `/usr/bin/true` inside it, because `simctl list` reporting `Booted` is a
/// state field and not an answer -- CoreSimulator can hold a simulator in
/// `Booted` while its runtime is wedged, and that read as usable and then
/// failed on the first operation.
///
/// Measured: 0.37 s for a booted simulator, 0.17 s to fail for a shut-down
/// one, ~0.11 s to fail for an absent physical device.
ReachabilityProbe probe_reachability(
    const std::string& device_id, const discovery::ProviderOptions& opts,
    model::DeviceForm form = model::DeviceForm::kPhysical);

/// Why an unusable iOS device is unusable, and what to do about it.
///
/// "offline" is a true statement and a useless one: it does not say whether
/// to reach for a cable, unlock the screen, trust the computer, or turn on
/// Developer Mode. Everything needed to answer that is already in the
/// listing and was being discarded.
///
/// Runs the live probe when `probe` is set, which costs about a tenth of a
/// second and is the only way to distinguish "not here" from "here but the
/// cached listing is stale".
std::vector<std::string> explain_unusable_device(
    const model::DeviceRef& device, const discovery::ProviderOptions& opts,
    bool probe);

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
