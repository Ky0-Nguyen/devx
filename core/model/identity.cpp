#include "core/model/identity.hpp"

namespace mpi::model {

const char* to_string(Platform p) {
  switch (p) {
    case Platform::kAndroid: return "android";
    case Platform::kIos: return "ios";
    case Platform::kUnknown: return "unknown";
  }
  return "unknown";
}
Platform platform_from_string(const std::string& s) {
  if (s == "android") return Platform::kAndroid;
  if (s == "ios") return Platform::kIos;
  return Platform::kUnknown;
}
const char* to_string(IdentifierKind k) {
  switch (k) {
    case IdentifierKind::kPackageName: return "package_name";
    case IdentifierKind::kBundleId: return "bundle_id";
    case IdentifierKind::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(DeviceForm f) {
  switch (f) {
    case DeviceForm::kPhysical: return "physical";
    case DeviceForm::kSimulator: return "simulator";
    case DeviceForm::kEmulator: return "emulator";
    case DeviceForm::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(TrustState t) {
  switch (t) {
    case TrustState::kAuthorized: return "authorized";
    case TrustState::kUnauthorized: return "unauthorized";
    case TrustState::kUntrusted: return "untrusted";
    case TrustState::kLocked: return "locked";
    case TrustState::kOffline: return "offline";
    case TrustState::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(ConnectionType c) {
  switch (c) {
    case ConnectionType::kUsb: return "usb";
    case ConnectionType::kWireless: return "wireless";
    case ConnectionType::kLocal: return "local";
    case ConnectionType::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(OwnershipEvidence e) {
  switch (e) {
    case OwnershipEvidence::kProviderAttributed: return "provider_attributed";
    case OwnershipEvidence::kUidAndProcessName: return "uid_and_process_name";
    case OwnershipEvidence::kAmbiguous: return "ambiguous";
    case OwnershipEvidence::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(RuntimeState s) {
  switch (s) {
    case RuntimeState::kRunning: return "running";
    case RuntimeState::kNotRunning: return "not_running";
    case RuntimeState::kSuspended: return "suspended";
    case RuntimeState::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(DiscoveryScope s) {
  switch (s) {
    case DiscoveryScope::kCompleteForProvider: return "complete_for_provider";
    case DiscoveryScope::kPartial: return "partial";
    case DiscoveryScope::kUnknown: return "unknown";
  }
  return "unknown";
}
const char* to_string(ProfilingAvailability a) {
  switch (a) {
    case ProfilingAvailability::kAvailable: return "available";
    case ProfilingAvailability::kLimited: return "limited";
    case ProfilingAvailability::kPermissionRequired: return "permission_required";
    case ProfilingAvailability::kUnavailable: return "unavailable";
    case ProfilingAvailability::kUnknown: return "unknown";
  }
  return "unknown";
}

json::Value DeviceRef::to_json() const {
  json::Value v = json::Value::object();
  v.set("platform", json::Value::string(to_string(platform)));
  v.set("device_id", json::Value::string(device_id));
  v.set("display_name", json::Value::string(display_name));
  v.set("model", json::Value::string(model));
  v.set("os_version", json::Value::string(os_version));
  v.set("form", json::Value::string(to_string(form)));
  v.set("trust", json::Value::string(to_string(trust)));
  v.set("connection", json::Value::string(to_string(connection)));
  v.set("provider", json::Value::string(provider));
  v.set("observed_at", json::Value::string(observed_at));
  v.set("boot_id", boot_id.empty() ? json::Value::null()
                                   : json::Value::string(boot_id));
  // Null rather than a copy of device_id when there is nothing to
  // distinguish: a reader must be able to tell "this device has two names"
  // from "these two fields happen to agree".
  v.set("hardware_udid", hardware_udid.empty()
                             ? json::Value::null()
                             : json::Value::string(hardware_udid));
  return v;
}

std::string ApplicationKey::canonical() const {
  std::string s = std::string(to_string(platform)) + "|" + device_id + "|" +
                  app_identifier;
  if (android_user_id.has_value()) {
    s += "|user=" + std::to_string(*android_user_id);
  }
  return s;
}

json::Value ApplicationKey::to_json() const {
  json::Value v = json::Value::object();
  v.set("platform", json::Value::string(to_string(platform)));
  v.set("device_id", json::Value::string(device_id));
  v.set("app_identifier", json::Value::string(app_identifier));
  v.set("identifier_kind", json::Value::string(to_string(identifier_kind)));
  v.set("android_user_id", android_user_id.has_value()
                               ? json::Value::integer(*android_user_id)
                               : json::Value::null());
  v.set("canonical", json::Value::string(canonical()));
  return v;
}

std::string ProcessInstance::canonical() const {
  std::string s = app.canonical() + "|pid=" + std::to_string(pid) + "|start=" +
                  process_start_time;
  if (!boot_id.empty()) s += "|boot=" + boot_id;
  return s;
}

json::Value ProcessInstance::to_json() const {
  json::Value v = json::Value::object();
  v.set("application_key", app.to_json());
  v.set("pid", json::Value::integer(pid));
  v.set("process_start_time", process_start_time.empty()
                                  ? json::Value::null()
                                  : json::Value::string(process_start_time));
  v.set("process_name", json::Value::string(process_name));
  v.set("uid", uid.has_value() ? json::Value::integer(*uid) : json::Value::null());
  v.set("is_primary", json::Value::boolean(is_primary));
  v.set("ownership_evidence", json::Value::string(to_string(ownership)));
  v.set("ownership_note", json::Value::string(ownership_note));
  v.set("counts_toward_app_totals",
        json::Value::boolean(counts_toward_app_totals()));
  v.set("process_instance_id", json::Value::string(canonical()));
  return v;
}

json::Value AppEntry::to_json() const {
  json::Value v = json::Value::object();
  v.set("application_key", key.to_json());
  v.set("display_name", json::Value::string(display_name));
  v.set("runtime_state", json::Value::string(to_string(runtime_state)));
  v.set("visibility_scope", json::Value::string(to_string(visibility_scope)));
  json::Value procs = json::Value::array();
  for (const auto& p : processes) procs.push_back(p.to_json());
  v.set("process_instances", std::move(procs));
  v.set("profiling_availability", json::Value::string(to_string(profiling)));
  v.set("profiling_reason", json::Value::string(profiling_reason));
  v.set("profiling_recovery_action",
        json::Value::string(profiling_recovery_action));
  // `installed` is only meaningful when the provider actually reported it.
  v.set("installed", installed_known ? json::Value::boolean(installed)
                                     : json::Value::null());
  v.set("provider", json::Value::string(provider));
  v.set("observed_at", json::Value::string(observed_at));
  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  v.set("notes", std::move(n));
  return v;
}

json::Value DiscoverySnapshot::to_json() const {
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  v.set("taken_at", json::Value::string(taken_at));
  json::Value d = json::Value::array();
  for (const auto& x : devices) d.push_back(x.to_json());
  v.set("devices", std::move(d));
  json::Value a = json::Value::array();
  for (const auto& x : apps) a.push_back(x.to_json());
  v.set("apps", std::move(a));
  json::Value e = json::Value::array();
  for (const auto& x : provider_errors) e.push_back(json::Value::string(x));
  v.set("provider_errors", std::move(e));
  // Spec A12: an empty list is not the same answer as a failed enumeration.
  v.set("enumeration_failed", json::Value::boolean(enumeration_failed));
  return v;
}

}  // namespace mpi::model
