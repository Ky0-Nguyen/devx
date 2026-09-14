// Capability contract (spec section 4).
//
// Every capability answers with a status, the provider that answered, what it
// was probed against, and how to recover. There is no boolean "supported":
// `unknown` is a distinct answer from `unsupported`, and neither is `false`.
#pragma once

#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::model {

enum class CapabilityStatus {
  kAvailable,
  kLimited,
  kUnsupported,
  kPermissionDenied,
  kUnknown,
};

const char* to_string(CapabilityStatus s);
CapabilityStatus capability_status_from_string(const std::string& s);

// Whether the capability has actually been exercised against real hardware, or
// only probed / reasoned about. Spec section 4 requires tested vs not-tested to
// be a reported field, and spec section 0.15 requires unverified live capture
// to stay explicitly unverified.
enum class TestedState {
  kVerifiedOnPhysicalDevice,
  kVerifiedOnSimulatorOrEmulator,
  kProbedOnly,
  kNotTested,
};

const char* to_string(TestedState s);

struct Capability {
  // Stable identifier, e.g. "android.discovery.installed_apps".
  std::string id;
  std::string human_name;
  CapabilityStatus status = CapabilityStatus::kUnknown;

  std::string provider;          // "adb", "devicectl", "xctrace", "sdk", ...
  std::string provider_version;  // as reported by the tool, verbatim

  // Host / toolchain / device / OS prerequisites, each as a human sentence.
  std::vector<std::string> prerequisites;

  // ISO-8601 UTC instant at which the observation was made.
  std::string observed_at;

  // What was actually run or read, and what came back. Never a claim without a
  // probe behind it.
  std::string evidence;

  // What the capability does and does not cover when status is kLimited.
  std::string scope;
  std::vector<std::string> limitations;

  // Concrete next action for the user when status is not kAvailable.
  std::string recovery_action;

  TestedState tested = TestedState::kNotTested;

  json::Value to_json() const;
};

struct CapabilityMatrix {
  std::vector<Capability> capabilities;

  const Capability* find(const std::string& id) const;
  void upsert(Capability c);
  json::Value to_json() const;
};

}  // namespace mpi::model
