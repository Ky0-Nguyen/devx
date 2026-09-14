// Stable identity (spec section 3.6).
//
// Application key = platform + device_id + app_identifier + user/profile.
// Process key     = application_key + pid + process_start_time (+ boot id).
//
// A restart is a new instance; PID reuse must not merge another process into
// the target; the same identifier on Android and iOS stays separate.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::model {

enum class Platform { kAndroid, kIos, kUnknown };
const char* to_string(Platform p);
Platform platform_from_string(const std::string& s);

enum class IdentifierKind { kPackageName, kBundleId, kUnknown };
const char* to_string(IdentifierKind k);

// Physical vs simulated is never collapsed: spec section 4.1 and J18 forbid
// equating simulator numbers with device numbers.
enum class DeviceForm { kPhysical, kSimulator, kEmulator, kUnknown };
const char* to_string(DeviceForm f);

enum class TrustState {
  kAuthorized,
  kUnauthorized,
  kUntrusted,
  kLocked,
  kOffline,
  kUnknown,
};
const char* to_string(TrustState t);

enum class ConnectionType { kUsb, kWireless, kLocal, kUnknown };
const char* to_string(ConnectionType c);

struct DeviceRef {
  Platform platform = Platform::kUnknown;
  // Stable across reconnects: adb serial, or the CoreDevice/simulator UDID.
  std::string device_id;
  std::string model;
  std::string os_version;
  DeviceForm form = DeviceForm::kUnknown;
  TrustState trust = TrustState::kUnknown;
  ConnectionType connection = ConnectionType::kUnknown;
  std::string display_name;
  std::string provider;  // which adapter observed it
  std::string observed_at;
  // Boot / session identity when the provider exposes one. Used to invalidate
  // process identity across a reboot (spec B04).
  std::string boot_id;

  bool usable_for_capture() const {
    return trust == TrustState::kAuthorized;
  }
  json::Value to_json() const;
};

struct ApplicationKey {
  Platform platform = Platform::kUnknown;
  std::string device_id;
  std::string app_identifier;
  IdentifierKind identifier_kind = IdentifierKind::kUnknown;
  // Android multi-user / work profile. Absent on iOS.
  std::optional<int> android_user_id;

  std::string canonical() const;
  bool operator==(const ApplicationKey& o) const {
    return platform == o.platform && device_id == o.device_id &&
           app_identifier == o.app_identifier &&
           android_user_id == o.android_user_id;
  }
  json::Value to_json() const;
};

// How confident we are that a given process belongs to the selected app.
// Spec B06/B07/section 3.6 require ambiguous ownership to stay unassigned
// rather than contaminate app totals.
enum class OwnershipEvidence {
  // Provider explicitly attributes the process to the package/bundle.
  kProviderAttributed,
  // Matched by UID *and* package-owned process-name convention.
  kUidAndProcessName,
  // Only a shared UID or a name prefix matched: not sufficient on its own.
  kAmbiguous,
  kUnknown,
};
const char* to_string(OwnershipEvidence e);

struct ProcessInstance {
  ApplicationKey app;
  std::int32_t pid = 0;
  // Monotonic-ish start marker from the provider (jiffies, boot-relative
  // seconds, or ISO instant). Compared as an opaque string plus the numeric
  // form when parseable; a change means a different instance.
  std::string process_start_time;
  std::string process_name;
  std::optional<std::int32_t> uid;
  bool is_primary = false;  // main app process vs secondary/service
  OwnershipEvidence ownership = OwnershipEvidence::kUnknown;
  std::string ownership_note;
  std::string boot_id;

  // Only processes with non-ambiguous ownership contribute to app-scoped
  // aggregates. Ambiguous ones stay listed but unassigned.
  bool counts_toward_app_totals() const {
    return ownership == OwnershipEvidence::kProviderAttributed ||
           ownership == OwnershipEvidence::kUidAndProcessName;
  }
  std::string canonical() const;
  json::Value to_json() const;
};

enum class RuntimeState { kRunning, kNotRunning, kSuspended, kUnknown };
const char* to_string(RuntimeState s);

// How complete the provider's listing is. Spec section 3.2 forbids presenting
// a partial listing as every application on the device.
enum class DiscoveryScope { kCompleteForProvider, kPartial, kUnknown };
const char* to_string(DiscoveryScope s);

enum class ProfilingAvailability {
  kAvailable,
  kLimited,
  kPermissionRequired,
  kUnavailable,
  kUnknown,
};
const char* to_string(ProfilingAvailability a);

struct AppEntry {
  ApplicationKey key;
  std::string display_name;
  RuntimeState runtime_state = RuntimeState::kUnknown;
  DiscoveryScope visibility_scope = DiscoveryScope::kUnknown;
  std::vector<ProcessInstance> processes;
  ProfilingAvailability profiling = ProfilingAvailability::kUnknown;
  std::string profiling_reason;
  // Why deep profiling may be unavailable and what setup could resolve it.
  std::string profiling_recovery_action;
  bool installed_known = false;
  bool installed = false;
  std::string provider;
  std::string observed_at;
  std::vector<std::string> notes;

  json::Value to_json() const;
};

// One complete discovery observation, persisted with the session so the target
// identity at capture time can be explained later (spec section 14).
struct DiscoverySnapshot {
  std::string taken_at;
  std::vector<DeviceRef> devices;
  std::vector<AppEntry> apps;
  // Per-provider errors, kept separate from "the list was empty" (spec A12).
  std::vector<std::string> provider_errors;
  bool enumeration_failed = false;

  json::Value to_json() const;
};

}  // namespace mpi::model
