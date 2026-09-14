// The interface every platform adapter implements.
//
// Discovery, installed-app enumeration, running-process enumeration, and
// permission-to-attach are *separate* capabilities (spec section 0.3). An
// adapter that can list installed apps but not running processes says exactly
// that, and the resulting list carries runtime_state = unknown rather than
// not_running.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/model/capability.hpp"
#include "core/model/identity.hpp"
#include "core/util/cancel.hpp"

namespace mpi::discovery {

struct ProviderOptions {
  CancellationToken cancel;
  // Per-command timeout. Device tooling occasionally hangs; a bounded wait
  // keeps the UI responsive (spec section 15).
  int command_timeout_ms = 15000;
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual model::Platform platform() const = 0;
  virtual std::string name() const = 0;

  // Probes the host toolchain. Always safe to call; returns the capability
  // records for everything this adapter could or could not establish.
  virtual void probe(model::CapabilityMatrix& out,
                     const ProviderOptions& opts) const = 0;

  // Lists devices this adapter can see. Errors go into `errors` and do not
  // produce a fabricated empty list (spec A12).
  virtual std::vector<model::DeviceRef> list_devices(
      const ProviderOptions& opts, std::vector<std::string>& errors) const = 0;

  // Lists apps on one device. The returned entries must state their own
  // visibility scope and profiling availability honestly.
  virtual std::vector<model::AppEntry> list_apps(
      const model::DeviceRef& device, const ProviderOptions& opts,
      std::vector<std::string>& errors, bool& enumeration_failed) const = 0;

  // Resolves the live process instances for one app identifier, revalidating
  // identity at the moment of the call (spec A17, A24).
  virtual std::vector<model::ProcessInstance> resolve_processes(
      const model::DeviceRef& device, const model::ApplicationKey& app,
      const ProviderOptions& opts, std::vector<std::string>& errors) const = 0;
};

using ProviderPtr = std::shared_ptr<Provider>;

}  // namespace mpi::discovery
