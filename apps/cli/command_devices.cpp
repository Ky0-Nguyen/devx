#include <iomanip>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/rules/engine.hpp"
#include "core/rules/rule_registry.hpp"
#include "core/util/time.hpp"

namespace mpi::cli {

ExitCode cmd_devices(const Invocation& inv) {
  auto svc = make_discovery(inv.global);
  const auto po = provider_options(inv.global);
  const auto snap = svc.snapshot(po, /*include_apps=*/false);

  if (inv.global.json) {
    print_json(snap.to_json());
  } else {
    if (snap.devices.empty()) {
      if (snap.enumeration_failed) {
        // Spec A12: this is not the same statement as "no devices".
        std::cout << "Device enumeration FAILED. This is not the same as "
                     "\"no devices are connected\".\n\n";
      } else {
        std::cout << "No device discovered.\n\n"
                     "To connect one:\n"
                     "  Android -- enable Developer options and USB debugging, "
                     "connect by USB, and accept the authorization prompt.\n"
                     "  iOS     -- connect an iPhone or iPad, unlock it, trust "
                     "this computer, and enable Developer Mode under\n"
                     "             Settings > Privacy & Security.\n\n";
      }
    } else {
      std::cout << std::left << std::setw(10) << "PLATFORM" << std::setw(11)
                << "FORM" << std::setw(14) << "STATE" << std::setw(39)
                << "DEVICE ID" << std::setw(16) << "OS" << "NAME\n";
      for (const auto& d : snap.devices) {
        std::cout << std::left << std::setw(10) << model::to_string(d.platform)
                  << std::setw(11) << model::to_string(d.form) << std::setw(14)
                  << model::to_string(d.trust) << std::setw(39) << d.device_id
                  << std::setw(16)
                  << (d.os_version.empty() ? "unknown" : d.os_version)
                  << d.display_name << "\n";
      }
      std::cout << "\n";

      const auto sel = svc.select_device(snap);
      std::cout << sel.note << "\n";
      // A simulator present in the list must never be read as a device.
      bool any_sim = false;
      for (const auto& d : snap.devices) {
        if (d.form == model::DeviceForm::kSimulator ||
            d.form == model::DeviceForm::kEmulator) {
          any_sim = true;
        }
      }
      if (any_sim) {
        std::cout << "\nSimulator/emulator entries are listed separately on "
                     "purpose: their timings are not comparable to a physical "
                     "device and are never mixed into the same baseline.\n";
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

ExitCode cmd_rules(const Invocation& inv) {
  // Declared separately from execution so a reader can see every detector,
  // its phase, and its thresholds without running an analysis.
  json::Value root = json::Value::object();
  root.set("ruleset_version", json::Value::string(rules::ruleset_version()));
  root.set("engine_version", json::Value::string(rules::engine_version()));
  json::Value arr = json::Value::array();
  for (const auto& r : rules::all_rules()) arr.push_back(r->describe());
  root.set("rules", std::move(arr));

  if (inv.global.json) {
    print_json(root);
    return ExitCode::kOk;
  }

  std::cout << "Ruleset " << rules::ruleset_version() << " (engine "
            << rules::engine_version() << ")\n\n";
  for (const auto& r : rules::all_rules()) {
    std::cout << r->id() << " v" << r->version() << "  [" << r->delivery_phase()
              << "]  " << r->title() << "\n";
    std::cout << "  category: " << r->category() << "\n";
    std::cout << "  prerequisites:\n";
    for (const auto& p : r->prerequisites()) {
      std::cout << "    - " << p.id << ": " << p.description << "\n";
    }
    const auto th = r->thresholds();
    if (!th.empty()) {
      std::cout << "  thresholds:\n";
      for (const auto& t : th) {
        std::cout << "    - " << t.name << " = " << t.value << " " << t.unit
                  << "  [" << rules::to_string(t.origin) << "]\n"
                  << "      " << t.rationale << "\n";
      }
    }
    const auto fp = r->known_false_positives();
    if (!fp.empty()) {
      std::cout << "  known false positives:\n";
      for (const auto& f : fp) std::cout << "    - " << f << "\n";
    }
    std::cout << "\n";
  }
  std::cout << "Override a threshold with --threshold RULE.name=value on "
               "`mpi analyze`.\n";
  return ExitCode::kOk;
}

}  // namespace mpi::cli
