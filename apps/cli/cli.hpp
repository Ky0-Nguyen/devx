// `mpi` command-line interface.
//
// Exit codes are documented and distinguish the four outcomes spec section 14
// requires: a regression, a collection error, an unsupported operation, and an
// inconclusive result. CI mode never prompts, and an ambiguous target returns
// the choices rather than guessing (spec section 14).
#pragma once

#include <string>
#include <vector>

#include "core/discovery/discovery_service.hpp"
#include "core/util/cancel.hpp"

namespace mpi::cli {

// Documented exit codes. Anything a caller might branch on has its own value.
enum class ExitCode : int {
  kOk = 0,
  kUsage = 2,
  kRegressionDetected = 3,
  kInconclusive = 4,
  kCollectionError = 5,
  kUnsupportedOperation = 6,
  kAmbiguousTarget = 7,
  kCancelled = 8,
  kNotFound = 9,
};
int to_int(ExitCode c);
const char* describe(ExitCode c);

struct GlobalOptions {
  bool json = false;
  // In CI mode nothing prompts and nothing is inferred from a single
  // candidate; ambiguity is an error with the choices listed.
  bool ci = false;
  bool include_simulators = true;
  bool quiet = false;
  int timeout_ms = 15000;
  std::string device;
  std::string app;
  std::string platform;  // "android" | "ios", for disambiguation only
  std::string sessions_dir;
  CancellationToken cancel;
};

// Parsed argv: the subcommand, its positional arguments, and flags.
struct Invocation {
  std::string command;
  std::vector<std::string> positional;
  GlobalOptions global;
  // Flags not consumed by the global parser, for the subcommand to read.
  std::vector<std::pair<std::string, std::string>> flags;

  std::string flag(const std::string& name, const std::string& fallback = {}) const;
  bool has_flag(const std::string& name) const;
};

// Builds a discovery service with the adapters the options ask for.
discovery::DiscoveryService make_discovery(const GlobalOptions& opts);
discovery::ProviderOptions provider_options(const GlobalOptions& opts);

// Subcommands.
ExitCode cmd_devices(const Invocation& inv);
ExitCode cmd_boot(const Invocation& inv);
ExitCode cmd_apps(const Invocation& inv);
ExitCode cmd_preflight(const Invocation& inv);
ExitCode cmd_record(const Invocation& inv);
ExitCode cmd_analyze(const Invocation& inv);
ExitCode cmd_timeline(const Invocation& inv);
ExitCode cmd_compare(const Invocation& inv);
ExitCode cmd_export(const Invocation& inv);
ExitCode cmd_rules(const Invocation& inv);
ExitCode cmd_sdk_bridge(const Invocation& inv);

// Shared helpers.
void print_json(const json::Value& v);
void warn(const std::string& msg);
// Resolves the single device the invocation targets. Returns false and fills
// `code` when the target is absent or ambiguous.
bool resolve_device(const Invocation& inv, const model::DiscoverySnapshot& snap,
                    model::DeviceRef& out, ExitCode& code);

}  // namespace mpi::cli
