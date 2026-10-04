#include "adapters/layout/layout_capture.hpp"

#include <chrono>
#include <thread>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/view_dump.hpp"
#include "adapters/ios/view_probe_client.hpp"
#include "core/observe/observation_store.hpp"

namespace mpi::layout {
namespace {

Capture fail(Failure f, std::string error) {
  Capture c;
  c.failure = f;
  c.error = std::move(error);
  return c;
}

void say(const CaptureOptions& o, const std::string& msg) {
  if (o.progress) o.progress(msg);
}

Capture capture_ios(const model::DeviceRef& device, const std::string& app,
                    const CaptureOptions& o) {
  if (device.form != model::DeviceForm::kSimulator) {
    return fail(Failure::kUnsupported,
                "the layout probe works on iOS simulators only. A physical "
                "device's code signing refuses an injected library, and there "
                "is no other route to its view tree");
  }
  if (app.rfind("com.apple.", 0) == 0) {
    return fail(Failure::kUnsupported,
                "'" + app + "' is a system app; the simulator does not load an "
                "injected library into it");
  }
  Capture out;
  const std::string socket = ios::probe_socket_path(device.device_id, app);
  ios::ProbeFetch fetch;
  if (o.relaunch) {
    std::vector<std::string> searched;
    const auto probe = ios::find_view_probe(&searched);
    if (!probe) {
      std::string where;
      for (const auto& s : searched) where += "\n  " + s;
      return fail(Failure::kProbeMissing,
                  "the layout probe library was not found. It is built with "
                  "the project when the iOS simulator SDK is present. Looked "
                  "in:" + where);
    }
    say(o, "relaunching " + app + " with the layout probe: the app restarts, "
           "and its current state is lost");
    const auto launched = ios::launch_with_probe(
        device.device_id, app, *probe, socket,
        std::max(o.timeout, std::chrono::milliseconds(30000)), o.cancel);
    if (!launched.launched) {
      return fail(o.cancel.cancelled() ? Failure::kCancelled : Failure::kCollection,
                  "could not launch " + app + ": " + launched.error);
    }
    out.launched_pid = launched.pid;
    out.notes.push_back(app + " was relaunched with the layout probe (pid " +
                        std::to_string(launched.pid) + "); later snapshots "
                        "reuse it without relaunching");
    if (o.settle.count() > 0) {
      say(o, "launched (pid " + std::to_string(launched.pid) + "); waiting " +
             std::to_string(o.settle.count() / 1000) +
             " s for the first screen before reading it");
    }
    const auto until = std::chrono::steady_clock::now() + o.settle;
    while (std::chrono::steady_clock::now() < until && !o.cancel.cancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fetch = ios::fetch_probe_snapshot(socket, o.timeout, true, o.cancel);
  } else {
    fetch = ios::fetch_probe_snapshot(socket, o.timeout, false, o.cancel);
    if (fetch.answer == ios::ProbeAnswer::kNotLoaded) {
      return fail(Failure::kNotLoaded,
                  "the layout probe is not loaded in " + app + " on this "
                  "simulator. Relaunch the app with it (its current state is "
                  "lost); once loaded, later snapshots read whatever screen it "
                  "is on");
    }
  }
  if (fetch.answer != ios::ProbeAnswer::kAnswered) {
    return fail(o.cancel.cancelled() ? Failure::kCancelled : Failure::kCollection,
                std::string("the probe did not answer (") +
                    ios::to_string(fetch.answer) + "): " + fetch.error);
  }
  json::ParseError perr;
  const auto doc = json::parse(fetch.body, observe::probe_json_limits(), &perr);
  if (!doc) {
    return fail(Failure::kCollection, "the probe's answer is not JSON: " + perr.message);
  }
  std::string err;
  auto snap = observe::parse_ios_probe(*doc, &err);
  if (!snap) return fail(Failure::kCollection, err);
  if (snap->app_identifier != app) {
    return fail(Failure::kCollection, "the probe answered for '" +
                                          snap->app_identifier + "', not '" +
                                          app + "'");
  }
  snap->device_id = device.device_id;
  observe::apply_display_names(
      *snap, ios::demangle_swift_names(observe::mangled_class_names(*snap),
                                       std::chrono::milliseconds(10000)));
  out.report = observe::analyze_layout(std::move(*snap));
  return out;
}

Capture capture_android(const model::DeviceRef& device, const std::string& app,
                        const CaptureOptions& o) {
  Capture out;
  if (o.relaunch) {
    out.notes.push_back("relaunch is for the iOS probe; Android's hierarchy is "
                        "read without restarting anything, so it was not done");
  }
  const auto dump = android::dump_activity_top(android::default_adb_path(),
                                               device.device_id, o.timeout, o.cancel);
  if (!dump.ok) {
    return fail(o.cancel.cancelled() ? Failure::kCancelled : Failure::kCollection,
                dump.error);
  }
  std::string err;
  auto snap = observe::parse_android_dumpsys(dump.text, app, &err);
  if (!snap) return fail(Failure::kNotOnScreen, err);
  snap->device_id = device.device_id;
  // dumpsys prints no time of its own; the read is this moment.
  snap->taken_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  out.report = observe::analyze_layout(std::move(*snap));
  return out;
}

}  // namespace

const char* to_string(Failure f) {
  switch (f) {
    case Failure::kNone:         return "none";
    case Failure::kUnsupported:  return "unsupported";
    case Failure::kProbeMissing: return "probe_missing";
    case Failure::kNotLoaded:    return "probe_not_loaded";
    case Failure::kNotOnScreen:  return "not_on_screen";
    case Failure::kCollection:   return "collection_error";
    case Failure::kCancelled:    return "cancelled";
  }
  return "collection_error";
}

json::Value Capture::to_json(bool include_tree) const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok()));
  o.set("failure", json::Value::string(layout::to_string(failure)));
  if (!error.empty()) o.set("error", json::Value::string(error));
  json::Value n = json::Value::array();
  for (const auto& s : notes) n.push_back(json::Value::string(s));
  o.set("notes", std::move(n));
  if (launched_pid > 0) o.set("launched_pid", json::Value::integer(launched_pid));
  if (!saved_id.empty()) {
    json::Value saved = json::Value::object();
    saved.set("id", json::Value::string(saved_id));
    saved.set("path", json::Value::string(saved_path));
    o.set("saved", std::move(saved));
  }
  if (!save_error.empty()) o.set("save_error", json::Value::string(save_error));
  o.set("report", report ? report->to_json(include_tree) : json::Value::null());
  return o;
}

namespace {

// What a listing shows without opening the document.
json::Value summarize(const observe::LayoutReport& r) {
  json::Value s = json::Value::object();
  s.set("source", json::Value::string(observe::to_string(r.snapshot.source)));
  s.set("views", json::Value::integer(r.views));
  s.set("visible_views", json::Value::integer(r.visible_views));
  s.set("max_depth", json::Value::integer(r.max_depth));
  s.set("screens", json::Value::integer(static_cast<std::int64_t>(r.screens.size())));
  json::Value names = json::Value::array();
  for (const auto& sc : r.screens) {
    if (sc.on_screen) names.push_back(json::Value::string(sc.name));
  }
  s.set("on_screen", std::move(names));
  s.set("observations",
        json::Value::integer(static_cast<std::int64_t>(r.observations.size())));
  s.set("react_native", json::Value::boolean(r.react_native.detected));
  return s;
}

}  // namespace

Capture capture_layout(const model::DeviceRef& device, const std::string& app,
                       const CaptureOptions& options) {
  if (app.empty()) return fail(Failure::kUnsupported, "no app identifier was given");
  Capture c;
  switch (device.platform) {
    case model::Platform::kIos:     c = capture_ios(device, app, options); break;
    case model::Platform::kAndroid: c = capture_android(device, app, options); break;
    case model::Platform::kUnknown:
      return fail(Failure::kUnsupported, "the device's platform is unknown");
  }
  if (c.ok() && !options.save_to.empty()) {
    const auto saved = observe::save_observation(
        options.save_to, "layout", app, device.device_id, summarize(*c.report),
        c.report->to_json(/*include_tree=*/true));
    if (saved.ok) {
      c.saved_id = saved.id;
      c.saved_path = saved.path;
    } else {
      c.save_error = saved.error;
    }
  }
  return c;
}

}  // namespace mpi::layout
