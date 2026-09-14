// Discovery service: reconciles every provider into one inventory.
//
// Selection survives a refresh, an app never silently retargets, and a
// selection is revalidated immediately before recording (spec A16, A17, A24).
#pragma once

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
