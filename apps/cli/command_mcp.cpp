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
#include "core/observe/observation_store.hpp"
#include "adapters/android/device_input.hpp"
#include "adapters/browserstack/automate.hpp"
#include "adapters/intelligence/service.hpp"
#include "adapters/browserstack/browserstack.hpp"
#include "adapters/browserstack/profiling.hpp"
#include "adapters/android/emulator/avd.hpp"
#include "adapters/android/emulator/emulator_process.hpp"
#include "adapters/android/emulator/sdk_repository.hpp"
#include "core/capi/control.hpp"
#include "core/observe/screenshot.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

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

// A simulator UDID is a UUID; an adb serial never is.
bool looks_like_udid(const std::string& id) {
  if (id.size() != 36) return false;
  for (std::size_t i = 0; i < id.size(); i++) {
    const char c = id[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::string url_encode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

std::string json_error(const std::string& message) {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(false));
  o.set("error", json::Value::string(message));
  return o.dump(2);
}

std::string json_ok(const std::string& note) {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(true));
  if (!note.empty()) o.set("note", json::Value::string(note));
  return o.dump(2);
}

// Input works on Android through adb; an iOS simulator has no command-line
// route for touches, so it is refused with that said.
bool android_only(const std::string& device, std::string* message) {
  if (device.empty()) {
    *message = json_error("device_id is required; list_devices gives one");
    return false;
  }
  if (looks_like_udid(device)) {
    *message = json_error(
        "input on an iOS simulator is not supported: there is no command-line route for "
        "touches without installing a helper on it. Screenshots and capture_layout work.");
    return false;
  }
  return true;
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
      [dir](const json::Value& a) {
        // Flags: 1 read redux, 2 values, 16 watch, 32 wrap dispatch, 8 detail.
        int flags = 1 | 16;
        if (arg_bool(a, "detail", false)) flags |= 8;
        const std::string app = arg_str(a, "app_identifier");
        const std::string text = owned(mpi_inspect_json(
            app.c_str(), arg_int(a, "seconds", 15), arg_int(a, "metro_port", 8081),
            flags, "", "", ""));
        // Kept, like the CLI keeps it, so a later conversation can read it
        // with read_observation instead of attaching again.
        json::ParseError perr;
        auto doc = json::parse(text, &perr);
        if (!doc || !doc->is_object()) return text;
        const json::Value* attached = doc->find("debugger_attached");
        if (attached != nullptr && attached->is_bool() && attached->as_bool()) {
          const auto saved = observe::save_observation(
              dir, "inspect", app, "", observe::summarize_inspect(*doc), *doc);
          if (saved.ok) doc->set("saved_observation", json::Value::string(saved.id));
        }
        return doc->dump(2);
      });

  // ---- the DevX window -----------------------------------------------------

  add(server, "devx_window_state",
      "What the open DevX window is showing: its tab, the selected device and "
      "app, the open session and issue. Errors when DevX is not running.",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) { return capi::control_request("/v1/state", 3000).dump(2); });

  add(server, "devx_window_show",
      "Show something in the open DevX window, so the person can see what you "
      "are talking about: a tab, a session (and an issue in it), a saved "
      "observation such as a layout snapshot, a device and app selection, or "
      "an emulator. Changes only what the window shows -- nothing is recorded, "
      "started or changed on a device -- and the window tells the person an AI "
      "tool did it, with `reason` if you give one. Tabs: devices, apps, "
      "preflight, live, record, inspect, layout, emulator, sessions, issues, "
      "threads, timeline, compare, detectors, settings, help.",
      schema({{"tab", "Which tab to show."},
              {"session_id", "A session from list_sessions; opens it."},
              {"issue_id", "With session_id: the issue to select."},
              {"observation_id", "A layout observation from list_observations; shows it."},
              {"device_id", "Select this device."},
              {"app_identifier", "Select this app."},
              {"avd_id", "Show this emulator's screen in the Emulator tab."},
              {"reason", "One line the window shows: why you are showing this."}}),
      mcp::Effect::kReadOnly,
      [](const json::Value& a) {
        std::string q = "/v1/show?x=1";
        const std::vector<std::pair<const char*, const char*>> map = {
            {"tab", "tab"}, {"session_id", "session"}, {"issue_id", "issue"},
            {"observation_id", "observation"}, {"device_id", "device"},
            {"app_identifier", "app"}, {"avd_id", "avd"}, {"reason", "reason"}};
        for (const auto& [arg, param] : map) {
          const std::string v = arg_str(a, arg);
          if (!v.empty()) q += std::string("&") + param + "=" + url_encode(v);
        }
        return capi::control_request(q, 8000).dump(2);
      });

  // ---- the device's screen ---------------------------------------------------

  mcp::Tool shot;
  shot.name = "device_screenshot";
  shot.description =
      "A screenshot of a device or emulator (Android) or a booted iOS simulator, "
      "returned as an image you can look at, plus its size. Coordinates for "
      "device_tap and device_swipe are pixels of this full-size image. For "
      "where each element is, capture_layout gives every view's frame.";
  shot.input_schema = schema({{"device_id", "A device id from list_devices."}}, {"device_id"});
  shot.effect = mcp::Effect::kReadOnly;
  shot.run_content = [dir](const json::Value& a, bool* failed) {
    json::Value content = json::Value::array();
    const std::string device = arg_str(a, "device_id");
    observe::ShotOptions o;
    o.platform = looks_like_udid(device) ? model::Platform::kIos : model::Platform::kAndroid;
    o.device_id = device;
    std::error_code ec;
    const std::string shots = dir + "/device-shots";
    std::filesystem::create_directories(shots, ec);
    o.out_path = shots + "/" + device + "-" +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count()) +
                 ".png";
    const auto s = observe::capture_screen(o);
    if (!s.captured) {
      *failed = true;
      content.push_back(mcp::text_item("no screenshot: " + s.error));
      return content;
    }
    std::ifstream in(s.path, std::ios::binary);
    std::stringstream bytes;
    bytes << in.rdbuf();
    content.push_back(mcp::image_item(bytes.str(), "image/png"));
    content.push_back(mcp::text_item(device + ": " + std::to_string(s.width) + "x" +
                                     std::to_string(s.height) + " px, saved at " + s.path));
    return content;
  };
  server.add_tool(std::move(shot));

  add(server, "device_tap",
      "Tap an Android device or emulator at device pixels (the full-size "
      "screenshot's coordinates). Operates the app, so it changes its state.",
      schema({{"device_id", "An adb serial from list_devices."},
              {"x", "int:Pixels from the left."},
              {"y", "int:Pixels from the top."}},
             {"device_id", "x", "y"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        std::string msg, err;
        const std::string d = arg_str(a, "device_id");
        if (!android_only(d, &msg)) return msg;
        return android::input_tap(android::adb_for_input(), d, arg_int(a, "x", -1),
                                  arg_int(a, "y", -1), &err)
                   ? json_ok("")
                   : json_error(err);
      });

  add(server, "device_swipe",
      "Swipe on an Android device or emulator from one point to another over "
      "duration_ms (default 300); a long duration at one point is a long press.",
      schema({{"device_id", "An adb serial."},
              {"x1", "int:Start x."}, {"y1", "int:Start y."},
              {"x2", "int:End x."}, {"y2", "int:End y."},
              {"duration_ms", "int:How long the gesture takes. Default 300."}},
             {"device_id", "x1", "y1", "x2", "y2"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        std::string msg, err;
        const std::string d = arg_str(a, "device_id");
        if (!android_only(d, &msg)) return msg;
        return android::input_swipe(android::adb_for_input(), d, arg_int(a, "x1", 0),
                                    arg_int(a, "y1", 0), arg_int(a, "x2", 0),
                                    arg_int(a, "y2", 0), arg_int(a, "duration_ms", 300), &err)
                   ? json_ok("")
                   : json_error(err);
      });

  add(server, "device_type_text",
      "Type text into whatever has focus on an Android device or emulator. "
      "Printable ASCII only: `adb shell input text` drops anything else silently, "
      "so other characters are refused rather than lost.",
      schema({{"device_id", "An adb serial."}, {"text", "What to type."}},
             {"device_id", "text"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        std::string msg, err;
        const std::string d = arg_str(a, "device_id");
        if (!android_only(d, &msg)) return msg;
        return android::input_text(android::adb_for_input(), d, arg_str(a, "text"), &err)
                   ? json_ok("")
                   : json_error(err);
      });

  add(server, "device_key",
      "Press a key on an Android device or emulator: back, home, recents, enter, "
      "delete, tab, escape, power, volume_up, volume_down, menu.",
      schema({{"device_id", "An adb serial."}, {"key", "Which key."}}, {"device_id", "key"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        std::string msg, err;
        const std::string d = arg_str(a, "device_id");
        if (!android_only(d, &msg)) return msg;
        return android::input_key(android::adb_for_input(), d, arg_str(a, "key"), &err)
                   ? json_ok("")
                   : json_error(err);
      });

  // ---- Android emulators --------------------------------------------------------

  add(server, "android_avds",
      "Android virtual devices on this Mac (no Android Studio needed), with "
      "their screen size, window size class and system image, and which are "
      "running (with the adb serial the device tools take).",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) {
        json::Value arr = json::Value::array();
        const auto running = android::running_emulators();
        for (const auto& v : android::list_avds()) {
          json::Value o = v.to_json();
          for (const auto& r : running) {
            if (r.avd_id == v.id) o.set("running", r.to_json());
          }
          arr.push_back(std::move(o));
        }
        return arr.dump(2);
      });

  add(server, "emulator_start",
      "Boot an Android virtual device and wait until Android is up. Returns "
      "its adb serial for the device tools.",
      schema({{"avd_id", "An id from android_avds."},
              {"cold_boot", "bool:Ignore the saved snapshot."}},
             {"avd_id"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        android::LaunchOptions o;
        o.cold_boot = arg_bool(a, "cold_boot", false);
        const auto r = android::launch_emulator(android::locate_sdk().root,
                                                arg_str(a, "avd_id"), o);
        if (!r.ok) return json_error(r.error);
        return r.emulator.to_json().dump(2);
      });

  add(server, "emulator_stop",
      "Shut down a running Android virtual device.",
      schema({{"avd_id", "An id from android_avds."}}, {"avd_id"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        const auto r = android::find_running(arg_str(a, "avd_id"));
        if (!r) return json_error("not running");
        std::string err;
        return android::stop_emulator(android::locate_sdk().root, *r, &err) ? json_ok("")
                                                                            : json_error(err);
      });

  // ---- BrowserStack ------------------------------------------------------------

  add(server, "browserstack_devices",
      "Real devices available on the BrowserStack account DevX has credentials for "
      "(BROWSERSTACK_USERNAME / BROWSERSTACK_ACCESS_KEY, or the Keychain entry DevX's "
      "Emulator tab saves): device, os, os_version.",
      no_args(), mcp::Effect::kReadOnly,
      [](const json::Value&) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        return browserstack::get(*c, "app-automate/devices.json").to_json().dump(2);
      });

  add(server, "browserstack_sessions",
      "App Automate builds, or with build_id the sessions in one build (status, "
      "device, duration, video and log URLs). The answer is also kept as an "
      "observation, readable later with read_observation.",
      schema({{"build_id", "A build's hashed_id; omit for the recent builds."}}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        const std::string build = arg_str(a, "build_id");
        if (build.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) {
          return json_error("not a build id: " + build);
        }
        const auto r = browserstack::get(
            *c, build.empty() ? "app-automate/builds.json?limit=20"
                              : "app-automate/builds/" + build + "/sessions.json");
        json::Value out = r.to_json();
        if (r.ok) {
          json::Value summary = json::Value::object();
          summary.set("what", json::Value::string(build.empty() ? "builds" : "build " + build));
          const auto saved = observe::save_observation(dir, "browserstack", "app-automate", "",
                                                       summary, r.body);
          if (saved.ok) out.set("saved_observation", json::Value::string(saved.id));
        }
        return out.dump(2);
      });

  add(server, "browserstack_import_profiling",
      "BrowserStack's App Profiling of one App Automate session (CPU, memory, "
      "frames, ANRs, battery, I/O; a paid BrowserStack plan), written as a DevX "
      "session that list_sessions, get_issues and the timeline then read like any "
      "other. The measurements are BrowserStack's, and the session says so.",
      schema({{"session_id", "BrowserStack's session id (hashed_id)."},
              {"build_id", "Its build's hashed_id; read from the session when omitted."}},
             {"session_id"}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        browserstack::ProfilingRequest req;
        req.session_id = arg_str(a, "session_id");
        req.build_id = arg_str(a, "build_id");
        req.sessions_dir = dir;
        return browserstack::import_profiling(*c, req, CancellationToken::none())
            .to_json()
            .dump(2);
      });

  add(server, "browserstack_session_start",
      "Start an App Automate session on a real BrowserStack device with an app "
      "uploaded for app-automate (bs://...), with BrowserStack's settings: "
      "network_profile (e.g. 4g-lte-good, 3g-umts-good, no-network), "
      "gps_location (\"lat,lng\"), timezone, language, locale, orientation, "
      "biometric, camera_injection, app_profiling. Returns the session id for "
      "the other browserstack_session_* tools. Uses BrowserStack minutes until "
      "stopped (it ends itself after 300 s idle).",
      schema({{"app_url", "bs://... from browserstack_upload with product app-automate."},
              {"device", "As browserstack_devices names it, e.g. Google Pixel 9."},
              {"os_version", "e.g. 16.0."},
              {"platform", "android (default) or ios."},
              {"network_profile", "BrowserStack's network profile name."},
              {"gps_location", "lat,lng."},
              {"timezone", "e.g. Tokyo."},
              {"language", "e.g. fr."},
              {"locale", "e.g. FR."},
              {"orientation", "portrait or landscape."},
              {"biometric", "bool:Enable biometric injection."},
              {"camera_injection", "bool:Enable camera image injection."},
              {"app_profiling", "bool:Profile the app (CPU, memory, fps, ...) for import on stop."}},
             {"app_url", "device", "os_version"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        return browserstack::start_session(*c, browserstack::AutomateSpec::from_json(a))
            .to_json()
            .dump(2);
      });

  mcp::Tool bs_shot;
  bs_shot.name = "browserstack_session_screenshot";
  bs_shot.description =
      "The screen of a running BrowserStack App Automate session, as an image. "
      "Coordinates for browserstack_session_input are pixels of this image.";
  bs_shot.input_schema = schema({{"session_id", "From browserstack_session_start."}}, {"session_id"});
  bs_shot.effect = mcp::Effect::kReadOnly;
  bs_shot.run_content = [](const json::Value& a, bool* failed) {
    json::Value content = json::Value::array();
    const auto c = browserstack::find_credentials();
    if (!c) {
      *failed = true;
      content.push_back(mcp::text_item("no BrowserStack credentials on this Mac"));
      return content;
    }
    const auto s = browserstack::screenshot(*c, arg_str(a, "session_id"));
    if (!s.ok) {
      *failed = true;
      content.push_back(mcp::text_item("no screenshot: " + s.error));
      return content;
    }
    content.push_back(mcp::image_item(s.png, "image/png"));
    content.push_back(mcp::text_item(std::to_string(s.width) + "x" + std::to_string(s.height) + " px"));
    return content;
  };
  server.add_tool(std::move(bs_shot));

  add(server, "browserstack_session_input",
      "Operate the app in a BrowserStack App Automate session: tap (x, y), "
      "swipe (x, y to x2, y2), type text into what has focus, or press a key "
      "(back, home, enter). Coordinates are screenshot pixels.",
      schema({{"session_id", "From browserstack_session_start."},
              {"action", "tap, swipe, type or key."},
              {"x", "int:Pixels from the left."}, {"y", "int:Pixels from the top."},
              {"x2", "int:Swipe end x."}, {"y2", "int:Swipe end y."},
              {"text", "For type."}, {"key", "For key: back, home or enter."}},
             {"session_id", "action"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        const std::string sid = arg_str(a, "session_id");
        const std::string action = arg_str(a, "action");
        browserstack::Reply r;
        if (action == "tap") {
          r = browserstack::tap(*c, sid, arg_int(a, "x", 0), arg_int(a, "y", 0));
        } else if (action == "swipe") {
          r = browserstack::swipe(*c, sid, arg_int(a, "x", 0), arg_int(a, "y", 0),
                                  arg_int(a, "x2", 0), arg_int(a, "y2", 0));
        } else if (action == "type") {
          r = browserstack::type_text(*c, sid, arg_str(a, "text"));
        } else if (action == "key") {
          r = browserstack::press(*c, sid, arg_str(a, "key"));
        } else {
          return json_error("action is tap, swipe, type or key");
        }
        return r.ok ? json_ok("") : json_error(r.error);
      });

  add(server, "browserstack_session_stop",
      "End a BrowserStack App Automate session. With import_profiling, waits for "
      "BrowserStack's App Profiling of it and imports it as a DevX session.",
      schema({{"session_id", "From browserstack_session_start."},
              {"import_profiling", "bool:Import its App Profiling as a DevX session."}},
             {"session_id"}),
      mcp::Effect::kMutating,
      [dir](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        return browserstack::stop_session(*c, arg_str(a, "session_id"),
                                          arg_bool(a, "import_profiling", false) ? dir : "",
                                          CancellationToken::none())
            .to_json()
            .dump(2);
      });

  add(server, "browserstack_upload",
      "Upload an .apk, .aab or .ipa to BrowserStack (product app-live or "
      "app-automate). Sends the file to a third party, so it is an action.",
      schema({{"file", "Absolute path of the app file."},
              {"product", "app-live (default) or app-automate."}},
             {"file"}),
      mcp::Effect::kMutating,
      [](const json::Value& a) {
        const auto c = browserstack::find_credentials();
        if (!c) return json_error("no BrowserStack credentials on this Mac");
        return browserstack::upload(*c, arg_str(a, "product", "app-live"), arg_str(a, "file"))
            .to_json()
            .dump(2);
      });

  // ---- Intelligence: production, CI/CD and other external evidence ----------
  //
  // Everything here reads the local store, so it answers offline and needs
  // no provider credential. Only connector_sync reaches a provider, and only
  // with --allow-network.

  auto ws_of = [dir](const json::Value& a, std::string* err) {
    const std::string w = arg_str(a, "workspace");
    return w.empty() ? intelligence::default_workspace(dir, err) : w;
  };
  const char* kWs = "Workspace id; may be omitted when there is only one.";

  add(server, "intelligence_projects",
      "Intelligence workspaces on this Mac: each binds a repository and app ids to "
      "connectors (Sentry, GitLab CI, Firebase exports, JSONL imports). With each, "
      "when its connectors last synced successfully, so you can tell how fresh the "
      "local evidence is before reasoning over it.",
      no_args(), mcp::Effect::kReadOnly,
      [dir](const json::Value&) { return intelligence::workspaces(dir).dump(2); });

  add(server, "connector_status",
      "The connectors of a workspace: provider, settings (never a credential: only "
      "whether one is present), health (up_to_date, partial, needs_auth, error, "
      "never_synced, paused), last success and last attempt, cursor, and local "
      "storage use. A partial sync means the local cache is not complete.",
      schema({{"workspace", kWs}}), mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::integrations(dir, ws).dump(2);
      });

  add(server, "signals_search",
      "Search normalized local signals across providers: crashes, issues, metrics, "
      "pipelines, CI jobs, deploys, tests. Filters: provider, kind, severity, "
      "environment, release (key), version, commit (prefix), since/until (ISO 8601), "
      "text. Returns compact index entries, newest first, and freshness.",
      schema({{"workspace", kWs}, {"provider", "sentry, gitlab, firebase, jsonl..."},
              {"kind", "crash, issue, metric, pipeline, ci_job, deploy, test, release, event"},
              {"severity", "fatal, error, warning, info"}, {"environment", "e.g. production"},
              {"release", "A release key from release_overview, e.g. com.acme.app@5.4.0+54019"},
              {"version", "Release version"}, {"commit", "Commit SHA or prefix"},
              {"since", "ISO 8601"}, {"until", "ISO 8601"}, {"text", "Words in the title or id"},
              {"limit", "int:At most this many (default 50)."}}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        json::Value q = json::Value::object();
        for (const char* k : {"provider", "kind", "severity", "environment", "version", "commit",
                              "since", "until", "text"}) {
          const std::string v = arg_str(a, k);
          if (!v.empty()) q.set(k, json::Value::string(v));
        }
        const std::string rel = arg_str(a, "release");
        if (!rel.empty()) q.set("release_key", json::Value::string(rel));
        q.set("limit", json::Value::integer(arg_int(a, "limit", 50)));
        return intelligence::signals_query(dir, ws, q).dump(2);
      });

  add(server, "signal_read",
      "One signal in full: normalized fields first. With raw=true, also a bounded, "
      "redacted excerpt of the provider's raw evidence (for a failed CI job, the "
      "window around the root error the connector located, not the head of the "
      "log). Raw evidence can hold personal data; ask for it only when needed.",
      schema({{"workspace", kWs}, {"signal_id", "From signals_search."},
              {"raw", "bool:Include a redacted raw excerpt (default false)."}},
             {"signal_id"}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::signal_read(dir, ws, arg_str(a, "signal_id"), arg_bool(a, "raw", false))
            .dump(2);
      });

  add(server, "release_overview",
      "Without a release: every release the local evidence names, newest first, with "
      "crash, issue, CI-failure and session counts. With one: its identity (version, "
      "build, commit) and any conflicts between sources, a build -> test -> deploy -> "
      "first production signal timeline, evidence by provider, linked DevX and "
      "BrowserStack sessions, exact links and candidate links kept apart, and the "
      "evidence that is missing. Candidate links are timing or partial metadata: "
      "never call them causes.",
      schema({{"workspace", kWs}, {"release", "A release key; omit to list releases."}}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        const std::string rel = arg_str(a, "release");
        return (rel.empty() ? intelligence::releases(dir, ws) : intelligence::release(dir, ws, rel))
            .dump(2);
      });

  add(server, "release_compare",
      "Two releases side by side: signals by kind, crash and issue counts, event and "
      "affected-user totals (only where providers reported them), CI and test "
      "failures, issues only in the candidate, and metric percentile deltas for the "
      "same metric in both. Computed by DevX; a difference in evidence is not a "
      "measured cause.",
      schema({{"workspace", kWs}, {"base", "Baseline release key."},
              {"candidate", "Release key to compare."}},
             {"base", "candidate"}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::compare(dir, ws, arg_str(a, "base"), arg_str(a, "candidate")).dump(2);
      });

  add(server, "signal_related",
      "The correlation edges of one signal, exact and candidate apart, each with the "
      "one-sentence evidence it rests on, and the other evidence of its release.",
      schema({{"workspace", kWs}, {"signal_id", "From signals_search."}}, {"signal_id"}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::related(dir, ws, arg_str(a, "signal_id")).dump(2);
      });

  add(server, "code_context_for_signal",
      "Deterministic code context from the workspace's local git repository: the "
      "release's commit (or HEAD, said so, when that commit is not here), the "
      "signal's stack frames resolved to tracked files with the lines around each "
      "and blame, and the release's diff limited to those files. Bounded; paths "
      "outside the repository are never read.",
      schema({{"workspace", kWs}, {"signal_id", "From signals_search."}}, {"signal_id"}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::code_context(dir, ws, arg_str(a, "signal_id")).dump(2);
      });

  add(server, "intelligence_evidence_pack",
      "A compact, cited evidence pack for a release or a signal, built for you to "
      "reason over: freshness, small normalized facts, exact links, candidate links, "
      "bounded redacted raw excerpts, code context, missing evidence, and the local "
      "evidence ids every fact came from (cite them). Start here for 'why did this "
      "release regress' questions, then follow up with the narrower tools.",
      schema({{"workspace", kWs}, {"release", "A release key."},
              {"signal_id", "Or one signal (its release is added)."},
              {"question", "What you are trying to answer, passed through."}}),
      mcp::Effect::kReadOnly,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        json::Value scope = json::Value::object();
        for (const char* k : {"release", "signal_id", "question"}) {
          const std::string v = arg_str(a, k);
          if (!v.empty()) scope.set(k, json::Value::string(v));
        }
        return intelligence::evidence_pack(dir, ws, scope).dump(2);
      });

  add(server, "connector_sync",
      "Refresh a workspace's connectors from their providers (Sentry, GitLab...) into "
      "the local store, incrementally, and report what was written and whether the "
      "sync was complete or partial. Contacts external services with credentials "
      "DevX holds (never shown to you), so it needs --allow-network.",
      schema({{"workspace", kWs}, {"connector_id", "One connector; omit for all that are not paused."}}),
      mcp::Effect::kNetwork,
      [dir, ws_of](const json::Value& a) {
        std::string err;
        const std::string ws = ws_of(a, &err);
        if (ws.empty()) return json_error(err);
        return intelligence::sync(dir, ws, arg_str(a, "connector_id"), CancellationToken::none()).dump(2);
      });

  add(server, "list_observations",
      "Saved results that are not captures: layout snapshots (how a screen is "
      "built) and inspect observations (network, console, Redux). Newest "
      "first, each with a small summary. Every `mpi layout`, `mpi inspect`, "
      "DevX snapshot and capture_layout/observe_app call is kept here.",
      schema({{"kind", "\"layout\" or \"inspect\"; omit for both."},
              {"limit", "int:At most this many. Default 50."}}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        return owned(mpi_observations_json(dir.c_str(), arg_str(a, "kind").c_str(),
                                           arg_int(a, "limit", 50)));
      });

  add(server, "read_observation",
      "One saved observation in full. A layout snapshot includes every view "
      "(class, frame, hidden, depth, parent), the per-screen statistics, the "
      "navigation stacks and the React Native counts; an inspect observation "
      "includes every network exchange, console line and Redux record.",
      schema({{"id", "The id from list_observations."}}, {"id"}),
      mcp::Effect::kReadOnly,
      [dir](const json::Value& a) {
        return owned(mpi_observation_json(dir.c_str(), arg_str(a, "id").c_str()));
      });

  add(server, "capture_layout",
      "How the screen an app is showing right now is built: views per screen, "
      "nesting depth, hidden and off-screen views, navigation stacks, React "
      "Native screens mounted against showing. Android is read through "
      "dumpsys; an iOS simulator through the layout probe, which must already "
      "be loaded (relaunch_with_layout_probe loads it). Restarts nothing. The "
      "snapshot, every view included, is saved; the reply omits the tree, "
      "and read_observation returns it. Structure, not a measurement.",
      schema({{"device_id", "A device id from list_devices."},
              {"app_identifier", "Package name or bundle id."}},
             {"device_id", "app_identifier"}),
      mcp::Effect::kReadOnly,
      [dir, timeout_ms](const json::Value& a) {
        return owned(mpi_layout_json(arg_str(a, "device_id").c_str(),
                                     arg_str(a, "app_identifier").c_str(),
                                     /*relaunch=*/0, 0, /*include_tree=*/0, 1,
                                     timeout_ms, dir.c_str()));
      });

  add(server, "relaunch_with_layout_probe",
      "iOS simulator only: restart the app with the layout probe injected, "
      "then take a layout snapshot. The app's current state is lost. Later "
      "capture_layout calls reuse the probe without restarting.",
      schema({{"device_id", "A simulator UDID from list_devices."},
              {"app_identifier", "Bundle id."},
              {"settle_s", "int:Seconds to let the first screen render. Default 3."}},
             {"device_id", "app_identifier"}),
      mcp::Effect::kMutating,
      [dir, timeout_ms](const json::Value& a) {
        return owned(mpi_layout_json(arg_str(a, "device_id").c_str(),
                                     arg_str(a, "app_identifier").c_str(),
                                     /*relaunch=*/1, arg_int(a, "settle_s", 3) * 1000,
                                     /*include_tree=*/0, 1, timeout_ms, dir.c_str()));
      });
}

}  // namespace

ExitCode cmd_mcp(const Invocation& inv) {
  const bool allow_actions = inv.has_flag("allow-actions");
  const bool allow_network = inv.has_flag("allow-network");
  mcp::Server server(allow_actions, allow_network);
  register_tools(server, inv.global.sessions_dir, inv.global.timeout_ms);

  // stderr, never stdout: a stray byte on stdout is a parse error at the host.
  std::cerr << "mpi mcp: ready on stdio, " << server.tools().size()
            << " tool(s), "
            << (allow_actions ? "actions allowed" : "read-only")
            << (allow_network ? ", network refresh allowed" : "")
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
