// `mpi mcp` -- the Model Context Protocol server, over stdio.
//
// An editor or agent launches this as a subprocess and speaks JSON-RPC 2.0
// over its stdin and stdout, one message per line. That is the whole
// transport: no port, no socket, nothing listening. A local tool talking to a
// local editor needs none of it, and each of them would be an attack surface
// on a process that can read every capture on the machine.
//
// Two rules about the streams, both of which are easy to get wrong and fatal
// when wrong:
//
//   **stdout carries protocol and nothing else.** A stray line of logging
//   there is a parse error at the host, which shows up as the server
//   "crashing on startup" with nothing in it that says why. Everything
//   diagnostic goes to stderr, which hosts collect separately.
//
//   **stdin ending is a clean shutdown.** The host closes the pipe when the
//   user quits; that is not an error and must not be reported as one.
//
// The tools are the C ABI's own JSON documents, handed back unchanged. That
// is deliberate: `mpi_capi` exists precisely to be "one JSON document per
// call", the desktop app already consumes it, and re-summarising a report for
// a model would mean deciding on its behalf which fields matter -- including
// the coverage notes and the not-measured markers, which are the fields most
// worth keeping.
#include <iostream>
#include <string>

#include "apps/cli/cli.hpp"
#include "core/capi/mpi_capi.h"
#include "core/mcp/protocol.hpp"

namespace mpi::cli {
namespace {

/// Calls a C ABI function and takes ownership of its string.
///
/// Every `mpi_*_json` returns malloc'd memory the caller frees. Wrapping it
/// once means no tool can forget.
std::string owned(char* raw) {
  if (raw == nullptr) {
    return "{\"error\":\"the core returned nothing, which is a defect in this "
           "tool rather than an answer about your capture\"}";
  }
  std::string out(raw);
  mpi_string_free(raw);
  return out;
}

std::string arg_str(const json::Value& args, const char* key,
                    const std::string& fallback = {}) {
  if (!args.is_object()) return fallback;
  const json::Value* v = args.find(key);
  return v != nullptr && v->is_string() && !v->as_string().empty()
             ? v->as_string()
             : fallback;
}

int arg_int(const json::Value& args, const char* key, int fallback) {
  if (!args.is_object()) return fallback;
  const json::Value* v = args.find(key);
  return v != nullptr && v->is_number() ? static_cast<int>(v->as_int())
                                        : fallback;
}

bool arg_bool(const json::Value& args, const char* key, bool fallback) {
  if (!args.is_object()) return fallback;
  const json::Value* v = args.find(key);
  return v != nullptr && v->is_bool() ? v->as_bool() : fallback;
}

/// A schema with no arguments.
json::Value no_args() {
  json::Value s = json::Value::object();
  s.set("type", json::Value::string("object"));
  s.set("properties", json::Value::object());
  return s;
}

/// A schema of named string/integer/boolean properties.
json::Value schema(const std::vector<std::pair<std::string, std::string>>& props,
                   const std::vector<std::string>& required = {}) {
  json::Value p = json::Value::object();
  for (const auto& [name, desc] : props) {
    json::Value one = json::Value::object();
    // The type is encoded in the description's prefix so the catalogue stays
    // one table rather than three.
    const bool is_int = desc.rfind("int:", 0) == 0;
    const bool is_bool = desc.rfind("bool:", 0) == 0;
    one.set("type", json::Value::string(is_int ? "integer"
                                        : is_bool ? "boolean" : "string"));
    const std::size_t cut = is_int ? 4 : (is_bool ? 5 : 0);
    one.set("description", json::Value::string(desc.substr(cut)));
    p.set(name, std::move(one));
  }
  json::Value s = json::Value::object();
  s.set("type", json::Value::string("object"));
  s.set("properties", std::move(p));
  if (!required.empty()) {
    json::Value req = json::Value::array();
    for (const auto& r : required) req.push_back(json::Value::string(r));
    s.set("required", std::move(req));
  }
  return s;
}

void add(mcp::Server& server, const std::string& name,
         const std::string& description, json::Value input,
         mcp::Effect effect,
         std::function<std::string(const json::Value&)> run) {
  mcp::Tool t;
  t.name = name;
  t.description = description;
  t.input_schema = std::move(input);
  t.effect = effect;
  t.run = std::move(run);
  server.add_tool(std::move(t));
}

void register_tools(mcp::Server& server, const std::string& sessions_dir,
                    int timeout_ms) {
  const std::string dir = sessions_dir;

  add(server, "mpi_version",
      "Engine and ruleset version of this tool. Useful for saying which "
      "build produced a report.",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) { return owned(mpi_version_json()); });

  add(server, "list_sessions",
      "Every capture stored on this machine: id, target, when it ran, and "
      "what it holds. Start here -- a session id is what every other report "
      "tool takes.",
      no_args(), mcp::Effect::kReadOnly,
      [dir](const json::Value&) {
        return owned(mpi_sessions_json(dir.c_str()));
      });

  add(server, "read_session",
      "The whole report for one capture: the normalised trace, every "
      "detector's finding with its evidence and thresholds, coverage gaps, "
      "and the provenance of each source. Nothing is summarised -- a value "
      "that was not measured is absent or marked, never defaulted to zero, "
      "and that distinction is the point of the report.",
      schema({{"session_id", "The id from list_sessions."}}, {"session_id"}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        return owned(mpi_session_json(dir.c_str(),
                                      arg_str(a, "session_id").c_str()));
      });

  add(server, "session_timeline",
      "The capture's tracks, binned. A bin carries a state as well as a "
      "number: a blank bin means NOT MEASURED and never zero, so an idle "
      "app and an unmeasured interval are different answers.",
      schema({{"session_id", "The id from list_sessions."},
              {"bins", "int:How many bins across the capture. Default 40."}},
             {"session_id"}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        return owned(mpi_session_timeline_json(
            dir.c_str(), arg_str(a, "session_id").c_str(),
            arg_int(a, "bins", 40)));
      });

  add(server, "describe_detectors",
      "Every detector, its thresholds, and where each threshold came from -- "
      "a measured platform constant or a configurable heuristic. Read this "
      "before arguing about whether a finding is real.",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) { return owned(mpi_rules_json()); });

  add(server, "list_devices",
      "Android and iOS devices and simulators visible now, with the trust "
      "state of each. An empty list and a failed enumeration are reported "
      "differently: 'no devices' is a claim about the machine.",
      schema({{"include_simulators",
               "bool:Include simulators and emulators. Default true."}}),
      mcp::Effect::kReadOnly,
      [timeout_ms](const json::Value& a) {
        return owned(mpi_devices_json(
            arg_bool(a, "include_simulators", true) ? 1 : 0, timeout_ms));
      });

  add(server, "list_apps",
      "Apps on one device, by package name or bundle id.",
      schema({{"device_id", "A device id from list_devices."}},
             {"device_id"}),
      mcp::Effect::kReadOnly,
      [timeout_ms](const json::Value& a) {
        return owned(mpi_apps_json(arg_str(a, "device_id").c_str(), 1,
                                   timeout_ms));
      });

  add(server, "preflight",
      "What can and cannot be captured for one device and app, probed rather "
      "than assumed. Each capability reports available / limited / "
      "unsupported / permission_denied with the evidence, the prerequisite "
      "and how to fix it. Run this before blaming a capture for coming back "
      "empty.",
      schema({{"device_id", "A device id from list_devices."},
              {"app_identifier", "Package name or bundle id."}},
             {"device_id", "app_identifier"}),
      mcp::Effect::kReadOnly,
      [timeout_ms](const json::Value& a) {
        return owned(mpi_preflight_json(
            arg_str(a, "device_id").c_str(),
            arg_str(a, "app_identifier").c_str(), 1, timeout_ms));
      });

  add(server, "compare_runsets",
      "Two run sets compared, with the gate status stated before any verdict: "
      "too few runs or too much variance is reported as inconclusive rather "
      "than as a regression.",
      schema({{"baseline_path", "Path to the baseline run-set JSON."},
              {"candidate_path", "Path to the candidate run-set JSON."}},
             {"baseline_path", "candidate_path"}),
      mcp::Effect::kReadOnly,
      [](const json::Value& a) {
        // Zeroes mean "the engine's own defaults": the gate thresholds are
        // a policy decision and not one a model should be inventing per
        // call. Someone who wants different ones uses `mpi compare`.
        return owned(mpi_compare_json(arg_str(a, "baseline_path").c_str(),
                                      arg_str(a, "candidate_path").c_str(),
                                      0, 0.0, 0.0, 0.0));
      });

  add(server, "inspect_targets",
      "Which running apps can be observed through the inspector a React "
      "Native debug build already runs, found through Metro. An empty list "
      "means nothing is attached -- a release build runs no inspector -- and "
      "is not a statement about the app being idle.",
      schema({{"metro_port", "int:Metro's port. Default 8081."}}),
      mcp::Effect::kReadOnly,
      [](const json::Value& a) {
        return owned(mpi_inspect_targets_json(arg_int(a, "metro_port", 8081)));
      });

  add(server, "analyze_trace",
      "Analyse a trace file directly, without a session package. Accepts "
      "this tool's normalised format, a Hermes profile, a Chrome "
      "trace-event file, or an xctrace export.",
      schema({{"trace_path", "Path to the trace file."}}, {"trace_path"}),
      mcp::Effect::kReadOnly,
      [](const json::Value& a) {
        return owned(mpi_analyze_trace_json(arg_str(a, "trace_path").c_str()));
      });

  add(server, "list_boot_targets",
      "Simulators and emulators that could be started. An AVD name is not a "
      "device id: the id only exists once one is running.",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) { return owned(mpi_boot_targets_json()); });

  // ---- Everything below changes something. ----

  add(server, "record_capture",
      "Record a new capture and analyse it. Starts processes on the device "
      "and writes a session package to disk.",
      schema({{"device_id", "A device id from list_devices."},
              {"app_identifier", "Package name or bundle id."},
              {"duration_s", "int:Seconds to record. Default 5."}},
             {"device_id", "app_identifier"}),
      mcp::Effect::kMutating,
      [dir, timeout_ms](const json::Value& a) {
        return owned(mpi_record_json(
            dir.c_str(), arg_str(a, "device_id").c_str(),
            arg_str(a, "app_identifier").c_str(),
            arg_int(a, "duration_s", 5), /*sample_hz=*/200,
            /*frames=*/1, /*cpu=*/1, /*memory=*/1,
            /*reset_frame_history=*/0, /*scheduling=*/0, /*heap=*/0,
            timeout_ms));
      });

  add(server, "boot_device",
      "Start a simulator or emulator and wait for it. Changes this machine's "
      "state. 'started' and 'ready' are reported separately, because a "
      "device that has appeared is not yet one that answers.",
      schema({{"identifier", "An AVD name or simulator UDID from "
                             "list_boot_targets."},
              {"ready_timeout_s", "int:How long to wait. Default 180."}},
             {"identifier"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        return owned(mpi_boot_json(arg_str(a, "identifier").c_str(),
                                   arg_int(a, "ready_timeout_s", 180)));
      });

  add(server, "observe_app",
      "Observe a running React Native debug build for a window: its network "
      "calls, console output and Redux store, read through the inspector the "
      "app already runs. Attaches a debugger, so nothing it returns is a "
      "performance measurement.",
      schema({{"app_identifier", "Bundle id or package name."},
              {"seconds", "int:How long to observe. Default 15."},
              {"metro_port", "int:Metro's port. Default 8081."},
              {"detail", "bool:Capture request headers and bodies. This is "
                         "where bearer tokens are."}},
             {"app_identifier"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        // Flags: 1 read redux, 2 values, 16 watch, 32 wrap dispatch, 8 detail.
        int flags = 1 | 16;
        if (arg_bool(a, "detail", false)) flags |= 8;
        return owned(mpi_inspect_json(arg_str(a, "app_identifier").c_str(),
                                      arg_int(a, "seconds", 15),
                                      arg_int(a, "metro_port", 8081), flags,
                                      "", "", ""));
      });
}

}  // namespace

ExitCode cmd_mcp(const Invocation& inv) {
  const bool allow_actions = inv.has_flag("allow-actions");
  mcp::Server server(allow_actions);
  register_tools(server, inv.global.sessions_dir, inv.global.timeout_ms);

  // stderr, never stdout: a stray byte on stdout is a parse error at the host.
  std::cerr << "mpi mcp: ready on stdio, " << server.tools().size()
            << " tool(s), "
            << (allow_actions ? "actions allowed" : "read-only")
            << ". Protocol " << mcp::kProtocolVersion << ".\n";

  std::string line;
  while (std::getline(std::cin, line)) {
    // Blank lines between messages are legal framing noise.
    if (line.empty()) continue;
    json::ParseError perr;
    auto doc = json::parse(line, json::Limits{}, &perr);
    if (!doc.has_value()) {
      const json::Value err = mcp::error_response(
          json::Value::null(), mcp::ErrorCode::kParseError,
          "could not parse that line as JSON: " + perr.message);
      std::cout << err.dump() << "\n" << std::flush;
      continue;
    }
    const auto response = server.handle(*doc);
    // No response for a notification: JSON-RPC forbids one, and sending it
    // anyway can make a host drop the connection.
    if (!response.has_value()) continue;
    std::cout << response->dump() << "\n" << std::flush;
  }

  // stdin closed: the host quit. A clean shutdown, not a failure.
  return ExitCode::kOk;
}

}  // namespace mpi::cli
