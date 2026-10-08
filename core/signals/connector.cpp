#include "core/signals/connector.hpp"

#include <mutex>

namespace mpi::signals {
namespace {

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, ConnectorFactory>& registry() {
  static std::map<std::string, ConnectorFactory> r;
  return r;
}

json::Value notes_json(const std::vector<std::string>& notes) {
  json::Value a = json::Value::array();
  for (const auto& n : notes) a.push_back(json::Value::string(n));
  return a;
}

}  // namespace

json::Value ConnectorInfo::to_json() const {
  json::Value o = json::Value::object();
  o.set("provider", json::Value::string(provider));
  o.set("display_name", json::Value::string(display_name));
  o.set("category", json::Value::string(category));
  o.set("egress", json::Value::string(egress));
  o.set("credential_help", json::Value::string(credential_help));
  o.set("credential_env", json::Value::string(credential_env));
  o.set("credential_service", json::Value::string(credential_service));
  o.set("credential_optional", json::Value::boolean(credential_optional));
  json::Value s = json::Value::array();
  for (const auto& [name, help] : settings) {
    json::Value one = json::Value::object();
    one.set("name", json::Value::string(name));
    one.set("help", json::Value::string(help));
    s.push_back(std::move(one));
  }
  o.set("settings", std::move(s));
  return o;
}

json::Value ConnectorCapabilities::to_json() const {
  json::Value o = json::Value::object();
  const std::pair<const char*, bool> flags[] = {
      {"issues", issues},       {"crashes", crashes},     {"metrics", metrics},
      {"traces", traces},       {"ci_pipelines", ci_pipelines}, {"ci_jobs", ci_jobs},
      {"deployments", deployments}, {"tests", tests},   {"artifacts", artifacts},
      {"webhooks", webhooks},   {"incremental_pull", incremental_pull}, {"events", events}, {"network", network}};
  for (const auto& [k, v] : flags) o.set(k, json::Value::boolean(v));
  return o;
}

json::Value ValidateResult::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!account.empty()) o.set("account", json::Value::string(account));
  o.set("notes", notes_json(notes));
  return o;
}

json::Value DiscoverResult::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  o.set("resources", resources);
  o.set("notes", notes_json(notes));
  return o;
}

json::Value ConnectorHealth::to_json() const {
  json::Value o = json::Value::object();
  o.set("state", json::Value::string(state));
  if (!detail.empty()) o.set("detail", json::Value::string(detail));
  return o;
}

void register_connector(const std::string& provider, ConnectorFactory factory) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry()[provider] = std::move(factory);
}

std::unique_ptr<SignalConnector> make_connector(const std::string& provider) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  const auto it = registry().find(provider);
  if (it == registry().end()) return nullptr;
  return it->second();
}

std::vector<std::string> registered_providers() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  std::vector<std::string> out;
  for (const auto& [k, _] : registry()) out.push_back(k);
  return out;
}

CredentialSource credential_source(const ConnectorConfig& config, const ConnectorInfo& info,
                                   bool env_allowed) {
  CredentialSource s;
  if (info.credential_service.empty() && info.credential_env.empty()) return s;  // needs none
  if (env_allowed) s.env_var = info.credential_env;
  if (!config.credential_ref.empty()) {
    s.keychain_services = {config.credential_ref};
  } else if (!info.credential_service.empty()) {
    s.keychain_services = {info.credential_service + "." + config.id, info.credential_service};
  }
  return s;
}

}  // namespace mpi::signals
