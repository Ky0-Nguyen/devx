#include <iomanip>
#include <iostream>

#include "adapters/android/adb_adapter.hpp"
#include "apps/cli/cli.hpp"
#include "core/model/build.hpp"

namespace mpi::cli {

ExitCode cmd_preflight(const Invocation& inv) {
  auto svc = make_discovery(inv.global);
  const auto po = provider_options(inv.global);

  // Probe the toolchain first: it is what decides whether anything else is
  // even answerable.
  const auto caps = svc.probe(po);
  const auto snap = svc.snapshot(po, /*include_apps=*/!inv.global.app.empty());

  model::DeviceRef device;
  ExitCode code = ExitCode::kOk;
  const bool have_device = resolve_device(inv, snap, device, code);

  // Eligibility for a benchmark is evaluated from whatever build facts we can
  // establish. With no SDK and no build manifest, that is deliberately little,
  // and the verdict is insufficient_evidence rather than a pass.
  model::BuildProfile build;
  if (have_device) {
    model::BuildFact form;
    form.key = "device.form";
    form.value = model::to_string(device.form);
    form.source = model::FactSource::kDeviceProvider;
    form.observed_at = snap.taken_at;
    form.basis = "reported by " + device.provider;
    build.upsert(std::move(form));

    model::BuildFact os;
    os.key = "device.os_version";
    os.value = device.os_version;
    os.source = model::FactSource::kDeviceProvider;
    os.observed_at = snap.taken_at;
    build.upsert(std::move(os));

    // Which *build* of the app is about to be measured. Two facts about the
    // device said nothing about that, so two captures either side of a
    // reinstall were indistinguishable (spec B11).
    if (!inv.global.app.empty() &&
        device.platform == model::Platform::kAndroid) {
      proc::Options build_po;
      build_po.timeout = std::chrono::milliseconds(inv.global.timeout_ms);
      build_po.cancel = inv.global.cancel;
      std::string build_error;
      if (!android::read_app_build_facts(android::default_adb_path(), device.device_id,
                                         inv.global.app, build_po, build,
                                         build_error) &&
          !build_error.empty()) {
        warn(build_error);
      }
    }
  }
  const auto diag_eligibility =
      model::evaluate_eligibility(build, model::MeasurementMode::kDiagnostic);
  const auto bench_eligibility =
      model::evaluate_eligibility(build, model::MeasurementMode::kBenchmark);

  const model::AppEntry* target = nullptr;
  std::vector<const model::AppEntry*> identifier_matches;
  if (!inv.global.app.empty()) {
    for (const auto& a : snap.apps) {
      if (a.key.app_identifier != inv.global.app) continue;
      if (have_device && a.key.device_id != device.device_id) continue;
      identifier_matches.push_back(&a);
    }
    if (identifier_matches.size() == 1) {
      target = identifier_matches.front();
    }
  }

  if (inv.global.json) {
    json::Value root = json::Value::object();
    root.set("schema_version", json::Value::string("2.0"));
    root.set("capabilities", caps.to_json());
    root.set("device", have_device ? device.to_json() : json::Value::null());
    root.set("target", target ? target->to_json() : json::Value::null());
    root.set("diagnostic_eligibility", diag_eligibility.to_json());
    root.set("benchmark_eligibility", bench_eligibility.to_json());
    root.set("build", build.to_json());
    print_json(root);
  } else {
    std::cout << "Capability preflight\n====================\n\n";
    std::cout << "ID                                      STATUS             "
                 "TESTED\n";
    for (const auto& c : caps.capabilities) {
      std::cout << std::left << std::setw(40) << c.id << std::setw(19)
                << model::to_string(c.status) << model::to_string(c.tested)
                << "\n";
    }
    std::cout << "\nDetail\n------\n";
    for (const auto& c : caps.capabilities) {
      std::cout << "\n" << c.id << " -- " << c.human_name << "\n";
      std::cout << "  status:   " << model::to_string(c.status);
      if (!c.provider_version.empty()) {
        std::cout << " (" << c.provider << " " << c.provider_version << ")";
      }
      std::cout << "\n";
      if (!c.evidence.empty()) std::cout << "  evidence: " << c.evidence << "\n";
      if (!c.scope.empty()) std::cout << "  scope:    " << c.scope << "\n";
      for (const auto& l : c.limitations) {
        std::cout << "  limit:    " << l << "\n";
      }
      for (const auto& p : c.prerequisites) {
        std::cout << "  needs:    " << p << "\n";
      }
      if (!c.recovery_action.empty()) {
        std::cout << "  fix:      " << c.recovery_action << "\n";
      }
      std::cout << "  tested:   " << model::to_string(c.tested) << "\n";
    }

    std::cout << "\nMeasurement eligibility\n-----------------------\n";
    std::cout << "diagnostic: " << model::to_string(diag_eligibility.status) << "\n";
    std::cout << "benchmark:  " << model::to_string(bench_eligibility.status) << "\n";
    for (const auto& r : bench_eligibility.reasons) {
      std::cout << "  - " << r << "\n";
    }
    std::cout << "\nA diagnostic session investigates behavior. It never "
                 "certifies release performance.\n";

    if (!inv.global.app.empty()) {
      std::cout << "\nTarget: " << inv.global.app << "\n";
      if (identifier_matches.empty()) {
        std::cout << "  NOT FOUND in the current app listing. The identifier "
                     "may be wrong, the app may not be installed, or the "
                     "listing may be partial.\n";
      } else if (identifier_matches.size() > 1) {
        std::cout << "  AMBIGUOUS: this identifier matches "
                  << identifier_matches.size()
                  << " entries (different devices or Android users). Narrow it "
                     "with --device.\n";
        for (const auto* a : identifier_matches) {
          std::cout << "    - " << a->key.canonical() << "\n";
        }
      } else {
        const auto& a = *target;
        std::cout << "  runtime state:        " << model::to_string(a.runtime_state)
                  << "\n";
        std::cout << "  profiling:            " << model::to_string(a.profiling)
                  << "\n";
        if (!a.profiling_reason.empty()) {
          std::cout << "    reason:             " << a.profiling_reason << "\n";
        }
        if (!a.profiling_recovery_action.empty()) {
          std::cout << "    fix:                " << a.profiling_recovery_action
                    << "\n";
        }
        std::cout << "  visibility scope:     "
                  << model::to_string(a.visibility_scope) << "\n";
        std::cout << "  process instances:    " << a.processes.size() << "\n";
        for (const auto& p : a.processes) {
          std::cout << "    pid " << p.pid << "  " << p.process_name << "\n";
          std::cout << "      ownership: " << model::to_string(p.ownership);
          std::cout << (p.counts_toward_app_totals()
                            ? "  (counts toward app totals)"
                            : "  (EXCLUDED from app totals)")
                    << "\n";
          if (!p.ownership_note.empty()) {
            std::cout << "      basis:     " << p.ownership_note << "\n";
          }
        }
        for (const auto& n : a.notes) std::cout << "  note: " << n << "\n";
      }
    } else {
      std::cout << "\nNo --app given, so no per-target capability was probed. "
                   "Profiling permission depends on the selected app's build, "
                   "not on the device alone.\n";
    }
  }

  if (inv.global.cancel.cancelled()) return ExitCode::kCancelled;
  if (!have_device) return code;
  if (!inv.global.app.empty()) {
    if (identifier_matches.empty()) return ExitCode::kNotFound;
    if (identifier_matches.size() > 1) return ExitCode::kAmbiguousTarget;
    if (target->profiling == model::ProfilingAvailability::kUnavailable) {
      return ExitCode::kUnsupportedOperation;
    }
  }
  return ExitCode::kOk;
}

}  // namespace mpi::cli
