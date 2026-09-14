#include "core/model/capability.hpp"

namespace mpi::model {

const char* to_string(CapabilityStatus s) {
  switch (s) {
    case CapabilityStatus::kAvailable: return "available";
    case CapabilityStatus::kLimited: return "limited";
    case CapabilityStatus::kUnsupported: return "unsupported";
    case CapabilityStatus::kPermissionDenied: return "permission_denied";
    case CapabilityStatus::kUnknown: return "unknown";
  }
  return "unknown";
}

CapabilityStatus capability_status_from_string(const std::string& s) {
  if (s == "available") return CapabilityStatus::kAvailable;
  if (s == "limited") return CapabilityStatus::kLimited;
  if (s == "unsupported") return CapabilityStatus::kUnsupported;
  if (s == "permission_denied") return CapabilityStatus::kPermissionDenied;
  return CapabilityStatus::kUnknown;
}

const char* to_string(TestedState s) {
  switch (s) {
    case TestedState::kVerifiedOnPhysicalDevice:
      return "verified_on_physical_device";
    case TestedState::kVerifiedOnSimulatorOrEmulator:
      return "verified_on_simulator_or_emulator";
    case TestedState::kProbedOnly: return "probed_only";
    case TestedState::kNotTested: return "not_tested";
  }
  return "not_tested";
}

namespace {
json::Value string_array(const std::vector<std::string>& in) {
  json::Value a = json::Value::array();
  for (const auto& s : in) a.push_back(json::Value::string(s));
  return a;
}
}  // namespace

json::Value Capability::to_json() const {
  json::Value v = json::Value::object();
  v.set("id", json::Value::string(id));
  v.set("human_name", json::Value::string(human_name));
  v.set("status", json::Value::string(to_string(status)));
  v.set("provider", json::Value::string(provider));
  v.set("provider_version", provider_version.empty()
                                ? json::Value::null()
                                : json::Value::string(provider_version));
  v.set("prerequisites", string_array(prerequisites));
  v.set("observed_at", json::Value::string(observed_at));
  v.set("evidence", json::Value::string(evidence));
  v.set("scope", json::Value::string(scope));
  v.set("limitations", string_array(limitations));
  v.set("recovery_action", json::Value::string(recovery_action));
  v.set("tested", json::Value::string(to_string(tested)));
  return v;
}

const Capability* CapabilityMatrix::find(const std::string& id) const {
  for (const auto& c : capabilities) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

void CapabilityMatrix::upsert(Capability c) {
  for (auto& existing : capabilities) {
    if (existing.id == c.id) {
      existing = std::move(c);
      return;
    }
  }
  capabilities.push_back(std::move(c));
}

json::Value CapabilityMatrix::to_json() const {
  json::Value a = json::Value::array();
  for (const auto& c : capabilities) a.push_back(c.to_json());
  json::Value v = json::Value::object();
  v.set("schema_version", json::Value::string("2.0"));
  v.set("capabilities", std::move(a));
  return v;
}

}  // namespace mpi::model
