#include <iomanip>
#include <iostream>

#include "apps/cli/cli.hpp"

namespace mpi::cli {

ExitCode cmd_apps(const Invocation& inv) {
  auto svc = make_discovery(inv.global);
  const auto po = provider_options(inv.global);
  const auto snap = svc.snapshot(po, /*include_apps=*/true);

  model::DeviceRef device;
  ExitCode code = ExitCode::kOk;
  if (!resolve_device(inv, snap, device, code)) return code;

  discovery::DiscoveryService::Filter filter;
  filter.running_only = inv.has_flag("running");
  filter.installed_only = inv.has_flag("installed");
  filter.profileable_only = inv.has_flag("profileable");
  filter.text = inv.flag("search");
  if (!inv.global.app.empty()) filter.text = inv.global.app;

  std::vector<model::AppEntry> device_apps;
  for (const auto& a : snap.apps) {
    if (a.key.device_id == device.device_id) device_apps.push_back(a);
  }
  const auto filtered =
      discovery::DiscoveryService::apply_filter(device_apps, filter);

  if (inv.global.json) {
    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
    root.set("device", device.to_json());
    // The listing's own scope and freshness travel with the data, so a
    // consumer can never mistake a partial list for the whole device.
    root.set("listed_at", json::Value::string(snap.taken_at));
    root.set("total_before_filter",
             json::Value::integer(static_cast<std::int64_t>(device_apps.size())));
    json::Value apps = json::Value::array();
    for (const auto& a : filtered) apps.push_back(a.to_json());
    root.set("apps", std::move(apps));
    json::Value errs = json::Value::array();
    for (const auto& e : snap.provider_errors) errs.push_back(json::Value::string(e));
    root.set("provider_errors", std::move(errs));
    root.set("enumeration_failed", json::Value::boolean(snap.enumeration_failed));
    print_json(root);
  } else {
    std::cout << "Device " << device.device_id << " ("
              << model::to_string(device.platform) << ", "
              << model::to_string(device.form) << ", " << device.display_name
              << ")\nObserved at " << snap.taken_at << "\n\n";

    if (device_apps.empty()) {
      if (snap.enumeration_failed) {
        std::cout << "App enumeration FAILED for this device. That is a "
                     "different answer from \"this device has no apps\".\n";
      } else {
        std::cout << "No app was listed for this device.\n";
      }
    } else {
      // Column widths must exceed the longest value they hold, or the fields
      // run together: "not_running" is itself 11 characters wide.
      std::cout << std::left << std::setw(13) << "STATE" << std::setw(21)
                << "PROFILING" << std::setw(23) << "SCOPE" << std::setw(7)
                << "PROCS" << "IDENTIFIER\n";
      for (const auto& a : filtered) {
        std::cout << std::left << std::setw(13)
                  << model::to_string(a.runtime_state) << std::setw(21)
                  << model::to_string(a.profiling) << std::setw(23)
                  << model::to_string(a.visibility_scope) << std::setw(7)
                  << a.processes.size() << a.key.app_identifier;
        if (a.display_name != a.key.app_identifier) {
          std::cout << "   (" << a.display_name << ")";
        }
        std::cout << "\n";
      }
      std::cout << "\n" << filtered.size() << " of " << device_apps.size()
                << " app(s) shown.\n";

      // State the semantics every time, because the whole picker depends on
      // the reader not conflating them.
      std::cout << "\nRuntime state: `running` does not mean foreground. "
                   "`unknown` means the provider could not observe the state "
                   "-- it does not mean not running.\n"
                   "Profiling availability is independent of runtime state: an "
                   "app can be running and still not be profileable.\n";

      bool any_partial = false;
      for (const auto& a : filtered) {
        if (a.visibility_scope == model::DiscoveryScope::kPartial) any_partial = true;
      }
      if (any_partial) {
        std::cout << "\nSome entries have a `partial` visibility scope: this "
                     "listing is not the complete set of apps on the device.\n";
      }

      // Explain the blockers, which is the actionable part.
      bool printed_header = false;
      for (const auto& a : filtered) {
        if (a.profiling == model::ProfilingAvailability::kAvailable) continue;
        if (a.profiling_reason.empty()) continue;
        if (!inv.global.app.empty() && a.key.app_identifier != inv.global.app) {
          continue;
        }
        if (!printed_header) {
          std::cout << "\nWhy some targets are not profileable:\n";
          printed_header = true;
        }
        std::cout << "  " << a.key.app_identifier << ": " << a.profiling_reason
                  << "\n";
        if (!a.profiling_recovery_action.empty()) {
          std::cout << "    -> " << a.profiling_recovery_action << "\n";
        }
      }
    }

    if (!snap.provider_errors.empty()) {
      std::cout << "\nProvider notes:\n";
      for (const auto& e : snap.provider_errors) std::cout << "  - " << e << "\n";
    }
  }

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  if (snap.enumeration_failed) return ExitCode::kCollectionError;
  return ExitCode::kOk;
}

}  // namespace mpi::cli
