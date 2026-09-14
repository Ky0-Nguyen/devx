// Discovery service: reconciles every provider into one inventory.
//
// Selection survives a refresh, an app never silently retargets, and a
// selection is revalidated immediately before recording (spec A16, A17, A24).
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/discovery/provider.hpp"
#include "core/model/capability.hpp"
#include "core/model/identity.hpp"

namespace mpi::discovery {

class DiscoveryService {
 public:
  void add_provider(ProviderPtr p) { providers_.push_back(std::move(p)); }

  // Probes every provider's toolchain.
  model::CapabilityMatrix probe(const ProviderOptions& opts) const;

  // One complete inventory snapshot, stored with the session so the target's
  // identity at capture time can be explained later.
  model::DiscoverySnapshot snapshot(const ProviderOptions& opts,
                                    bool include_apps) const;

  // Exactly one usable device is preselected; several require an explicit
  // choice; none is not an error (spec A04, A05).
  struct DeviceSelection {
    std::optional<model::DeviceRef> preselected;
    std::vector<model::DeviceRef> candidates;
    std::string note;
  };
  DeviceSelection select_device(const model::DiscoverySnapshot& snap) const;

  // Re-resolves the processes for a pinned target. Reports whether the target
  // is still present, so the UI can warn instead of retargeting.
  struct Revalidation {
    bool app_still_present = false;
    bool process_set_changed = false;
    std::vector<model::ProcessInstance> processes;
    std::vector<std::string> notes;
    std::vector<std::string> errors;
  };
  Revalidation revalidate(const model::DeviceRef& device,
                          const model::ApplicationKey& app,
                          const std::vector<model::ProcessInstance>& previous,
                          const ProviderOptions& opts) const;

  // Waits for a launched app's process to appear.
  //
  // `am start -W` returns once the activity reported being drawn, which is
  // before the process is reliably listable -- and a capture that begins
  // immediately pins nothing. This polls `revalidate` until the app is
  // present or the budget runs out, and says which of the two happened rather
  // than returning an empty set that reads like an app with no processes.
  struct ProcessWait {
    bool appeared = false;
    std::vector<model::ProcessInstance> processes;
    std::chrono::milliseconds waited{0};
    int polls = 0;
    bool cancelled = false;
    std::vector<std::string> notes;
  };
  ProcessWait wait_for_app_process(const model::DeviceRef& device,
                                   const model::ApplicationKey& app,
                                   std::chrono::milliseconds timeout,
                                   std::chrono::milliseconds poll_interval,
                                   const ProviderOptions& opts) const;

  // Resolves one device and one app without enumerating everything.
  //
  // `snapshot(include_apps=true)` lists every app on every usable device,
  // which on a host with a booted simulator and an emulator is a few hundred
  // entries and several seconds. That is the right cost for a picker and the
  // wrong cost for starting a capture, where the target is already known. This
  // walks devices (cheap) and then lists apps for the matched device only.
  struct TargetResolution {
    bool device_found = false;
    bool device_ambiguous = false;
    bool device_usable = false;
    bool app_found = false;
    bool app_ambiguous = false;
    bool enumeration_failed = false;
    model::DeviceRef device;
    model::AppEntry app;
    std::vector<model::DeviceRef> all_devices;
    std::vector<std::string> errors;
  };
  TargetResolution resolve_target(const std::string& device_id,
                                  const std::string& app_identifier,
                                  const ProviderOptions& opts) const;

  // Filters for the picker. Unavailable entries are never hidden by the
  // "profileable" filter -- they are kept and marked (spec A25).
  struct Filter {
    bool running_only = false;
    bool installed_only = false;
    bool profileable_only = false;
    std::string text;  // matches display name or identifier, case-insensitive
  };
  static std::vector<model::AppEntry> apply_filter(
      const std::vector<model::AppEntry>& apps, const Filter& f);

 private:
  std::vector<ProviderPtr> providers_;
};

}  // namespace mpi::discovery
