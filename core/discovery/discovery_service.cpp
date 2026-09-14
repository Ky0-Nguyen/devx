#include "core/discovery/discovery_service.hpp"

#include <algorithm>
#include <thread>
#include <cctype>

#include "core/util/time.hpp"

namespace mpi::discovery {
namespace {

std::string lower(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

}  // namespace

model::CapabilityMatrix DiscoveryService::probe(const ProviderOptions& opts) const {
  model::CapabilityMatrix m;
  for (const auto& p : providers_) {
    if (opts.cancel.cancelled()) break;
    p->probe(m, opts);
  }
  if (providers_.empty()) {
    model::Capability c;
    c.id = "discovery.providers";
    c.human_name = "Discovery providers";
    c.status = model::CapabilityStatus::kUnsupported;
    c.evidence = "no platform adapter is registered";
    c.observed_at = time_util::now_iso8601_utc();
    m.upsert(std::move(c));
  }
  return m;
}

model::DiscoverySnapshot DiscoveryService::snapshot(const ProviderOptions& opts,
                                                    bool include_apps) const {
  model::DiscoverySnapshot snap;
  snap.taken_at = time_util::now_iso8601_utc();

  std::size_t providers_failed = 0;
  for (const auto& p : providers_) {
    if (opts.cancel.cancelled()) break;
    std::vector<std::string> errors;
    auto devices = p->list_devices(opts, errors);
    const bool had_errors = !errors.empty();
    for (auto& e : errors) {
      snap.provider_errors.push_back(p->name() + ": " + e);
    }
    if (had_errors && devices.empty()) ++providers_failed;

    for (auto& d : devices) {
      if (include_apps && d.usable_for_capture()) {
        std::vector<std::string> app_errors;
        bool failed = false;
        auto apps = p->list_apps(d, opts, app_errors, failed);
        for (auto& e : app_errors) {
          snap.provider_errors.push_back(p->name() + "/" + d.device_id + ": " + e);
        }
        if (failed) ++providers_failed;
        for (auto& a : apps) snap.apps.push_back(std::move(a));
      }
      snap.devices.push_back(std::move(d));
    }
  }

  // Spec A12: an empty list is not the same as a failed enumeration. Only
  // claim failure when every provider failed and nothing came back.
  snap.enumeration_failed =
      !providers_.empty() && providers_failed == providers_.size() &&
      snap.devices.empty() && snap.apps.empty();
  return snap;
}

DiscoveryService::DeviceSelection DiscoveryService::select_device(
    const model::DiscoverySnapshot& snap) const {
  DeviceSelection sel;
  for (const auto& d : snap.devices) {
    if (d.usable_for_capture()) sel.candidates.push_back(d);
  }
  if (sel.candidates.size() == 1) {
    // Preselected, but capture is never started automatically (spec A05).
    sel.preselected = sel.candidates.front();
    sel.note =
        "exactly one usable device; preselected. Recording is not started "
        "automatically.";
  } else if (sel.candidates.empty()) {
    sel.note = snap.devices.empty()
                   ? "no device was discovered"
                   : std::to_string(snap.devices.size()) +
                         " device(s) discovered, but none is in a usable state";
  } else {
    sel.note = std::to_string(sel.candidates.size()) +
               " usable devices; an explicit choice is required";
  }
  return sel;
}

DiscoveryService::Revalidation DiscoveryService::revalidate(
    const model::DeviceRef& device, const model::ApplicationKey& app,
    const std::vector<model::ProcessInstance>& previous,
    const ProviderOptions& opts) const {
  Revalidation r;
  for (const auto& p : providers_) {
    if (p->platform() != device.platform) continue;
    r.processes = p->resolve_processes(device, app, opts, r.errors);
    r.app_still_present = !r.processes.empty();
    break;
  }
  if (r.processes.empty() && r.errors.empty()) {
    r.notes.push_back(
        "the app is no longer running, or no process could be attributed to "
        "it at this moment");
  }

  // Compare process identity, not just PIDs: a restart is a new instance even
  // when the PID happens to repeat (spec B02, B03).
  auto canonical_set = [](const std::vector<model::ProcessInstance>& v) {
    std::vector<std::string> out;
    for (const auto& p : v) out.push_back(p.canonical());
    std::sort(out.begin(), out.end());
    return out;
  };
  const auto before = canonical_set(previous);
  const auto after = canonical_set(r.processes);
  r.process_set_changed = before != after;
  if (r.process_set_changed && !previous.empty()) {
    r.notes.push_back(
        "the process set changed since selection: the app restarted, or a "
        "secondary process started or exited. The selected app identifier is "
        "unchanged; the tool has not retargeted.");
    for (const auto& p : r.processes) {
      const bool seen = std::find(before.begin(), before.end(), p.canonical()) !=
                        before.end();
      if (!seen) {
        r.notes.push_back("new process instance: pid " + std::to_string(p.pid) +
                          " (" + p.process_name + ")");
      }
    }
    for (const auto& p : previous) {
      const bool still = std::find(after.begin(), after.end(), p.canonical()) !=
                         after.end();
      if (!still) {
        r.notes.push_back("process instance gone: pid " + std::to_string(p.pid) +
                          " (" + p.process_name + ")");
      }
    }
  }
  // A reboot invalidates every prior process identity for the device.
  if (!previous.empty() && !device.boot_id.empty()) {
    for (const auto& p : previous) {
      if (!p.boot_id.empty() && p.boot_id != device.boot_id) {
        r.notes.push_back(
            "the device boot identity changed since selection; all previously "
            "resolved process identities are invalid");
        break;
      }
    }
  }
  return r;
}

DiscoveryService::TargetResolution DiscoveryService::resolve_target(
    const std::string& device_id, const std::string& app_identifier,
    const ProviderOptions& opts) const {
  TargetResolution out;

  // Devices first, from every provider. This is the cheap half: adb's listing
  // is a few hundred milliseconds and devicectl's a couple of seconds.
  const Provider* owner = nullptr;
  for (const auto& p : providers_) {
    if (opts.cancel.cancelled()) break;
    std::vector<std::string> errors;
    auto devices = p->list_devices(opts, errors);
    for (auto& e : errors) out.errors.push_back(p->name() + ": " + e);
    for (auto& d : devices) {
      if (d.device_id == device_id) {
        if (out.device_found) {
          // The same id on two platforms stays separate; picking one would be
          // exactly the silent retarget spec A15 forbids.
          out.device_ambiguous = true;
          out.app_found = false;
          return out;
        }
        out.device_found = true;
        out.device = d;
        owner = p.get();
      }
      out.all_devices.push_back(std::move(d));
    }
  }
  if (!out.device_found || owner == nullptr) return out;
  out.device_usable = out.device.usable_for_capture();
  if (!out.device_usable || app_identifier.empty()) return out;

  // Apps for the matched device only.
  std::vector<std::string> app_errors;
  bool failed = false;
  const auto apps = owner->list_apps(out.device, opts, app_errors, failed);
  for (auto& e : app_errors) {
    out.errors.push_back(owner->name() + "/" + out.device.device_id + ": " + e);
  }
  out.enumeration_failed = failed;

  std::size_t matches = 0;
  for (const auto& a : apps) {
    if (a.key.app_identifier != app_identifier) continue;
    ++matches;
    out.app = a;
  }
  out.app_found = matches == 1;
  out.app_ambiguous = matches > 1;
  return out;
}

std::vector<model::AppEntry> DiscoveryService::apply_filter(
    const std::vector<model::AppEntry>& apps, const Filter& f) {
  std::vector<model::AppEntry> out;
  const std::string needle = lower(f.text);
  for (const auto& a : apps) {
    // "Running only" keeps unknown-state entries: excluding them would be the
    // same mistake as rendering unknown as not_running (spec A11).
    if (f.running_only && a.runtime_state == model::RuntimeState::kNotRunning) {
      continue;
    }
    if (f.installed_only && a.installed_known && !a.installed) continue;
    if (f.profileable_only) {
      // Unavailable entries are kept and marked rather than hidden, so a user
      // can see why a target cannot be profiled (spec 3.2, A25).
      const bool clearly_unavailable =
          a.profiling == model::ProfilingAvailability::kUnavailable;
      if (clearly_unavailable) continue;
    }
    if (!needle.empty()) {
      const bool hit = lower(a.display_name).find(needle) != std::string::npos ||
                       lower(a.key.app_identifier).find(needle) != std::string::npos;
      if (!hit) continue;
    }
    out.push_back(a);
  }
  // Running and available entries sort first without removing the rest.
  std::stable_sort(out.begin(), out.end(),
                   [](const model::AppEntry& a, const model::AppEntry& b) {
                     auto rank = [](const model::AppEntry& x) {
                       int r = 0;
                       if (x.runtime_state == model::RuntimeState::kRunning) r -= 4;
                       else if (x.runtime_state == model::RuntimeState::kUnknown) r -= 1;
                       if (x.profiling == model::ProfilingAvailability::kAvailable) r -= 2;
                       else if (x.profiling == model::ProfilingAvailability::kUnavailable) r += 2;
                       return r;
                     };
                     const int ra = rank(a), rb = rank(b);
                     if (ra != rb) return ra < rb;
                     return a.key.app_identifier < b.key.app_identifier;
                   });
  return out;
}

DiscoveryService::ProcessWait DiscoveryService::wait_for_app_process(
    const model::DeviceRef& device, const model::ApplicationKey& app,
    std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval,
    const ProviderOptions& opts) const {
  ProcessWait out;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + timeout;

  for (;;) {
    if (opts.cancel.cancelled()) {
      out.cancelled = true;
      out.notes.push_back("cancelled while waiting for the app's process");
      break;
    }
    ++out.polls;
    const auto reval = revalidate(device, app, out.processes, opts);
    for (const auto& e : reval.errors) out.notes.push_back(e);
    if (reval.app_still_present && !reval.processes.empty()) {
      out.appeared = true;
      out.processes = reval.processes;
      break;
    }
    // Checked after the poll, so a zero timeout still gets one look: an app
    // that is already up should not need a waiting budget to be found.
    if (std::chrono::steady_clock::now() >= deadline) {
      out.notes.push_back(
          "the app did not appear as a running process within the wait budget; "
          "this is a timeout, not evidence that the app has no processes");
      break;
    }
    std::this_thread::sleep_for(poll_interval);
  }

  out.waited = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return out;
}

}  // namespace mpi::discovery
