#include "adapters/rn/inspector.hpp"

#include <chrono>

#include "core/observe/screenshot.hpp"

#include "core/net/loopback_http.hpp"
#include "core/net/websocket_client.hpp"
#include "core/util/json.hpp"

namespace mpi::rn {
namespace {

const std::string kEmpty;

const std::string& str(const json::Value& v, std::string_view key) {
  if (!v.is_object()) return kEmpty;
  const json::Value* f = v.find(key);
  return (f != nullptr && f->is_string()) ? f->as_string() : kEmpty;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// Strips the scheme and authority from a websocket URL.
///
/// Metro returns an absolute `ws://localhost:8081/inspector/debug?...`. The
/// client connects to loopback by construction, so only the path is used --
/// and taking the path rather than trusting the host is also what keeps a
/// target list from redirecting this tool at another machine.
std::string path_of(const std::string& ws_url) {
  std::size_t scheme = ws_url.find("://");
  if (scheme == std::string::npos) {
    return ws_url.empty() || ws_url[0] == '/' ? ws_url : "/" + ws_url;
  }
  std::size_t slash = ws_url.find('/', scheme + 3);
  if (slash == std::string::npos) return "/";
  return ws_url.substr(slash);
}

}  // namespace

TargetList list_targets(std::uint16_t metro_port) {
  TargetList out;
  net::GetResult res = net::loopback_get(metro_port, "/json/list");
  if (!res.ok) {
    out.error = res.error.empty()
        ? "Metro did not answer on 127.0.0.1:" + std::to_string(metro_port)
        : res.error;
    return out;
  }
  out.metro_reachable = true;
  json::ParseError perr;
  auto doc = json::parse(res.body, &perr);
  if (!doc.has_value() || !doc->is_array()) {
    out.error = "Metro's target list was not a JSON array";
    return out;
  }
  for (const json::Value& t : doc->items()) {
    InspectorTarget target;
    target.app_id = str(t, "appId");
    target.title = str(t, "title");
    target.description = str(t, "description");
    target.device_name = str(t, "deviceName");
    target.websocket_path = path_of(str(t, "webSocketDebuggerUrl"));
    if (target.websocket_path.empty()) continue;
    out.targets.push_back(std::move(target));
  }
  out.metro_answered_empty = out.targets.empty();
  return out;
}

const InspectorTarget* choose_target(const std::vector<InspectorTarget>& targets,
                                     const std::string& wanted_app_id) {
  const InspectorTarget* best = nullptr;
  int best_score = -1;
  for (const InspectorTarget& t : targets) {
    int score = 0;
    // Metro lists several entries per app; the one describing the runtime
    // connection is the one carrying Runtime and Network. The others are
    // auxiliary pages that answer far less.
    if (t.description.find("C++ connection") != std::string::npos) score += 4;
    if (t.description.find("Bridgeless") != std::string::npos) score += 1;
    if (!wanted_app_id.empty() && t.app_id == wanted_app_id) score += 8;
    if (!wanted_app_id.empty() && t.app_id != wanted_app_id) score -= 16;
    if (score > best_score) {
      best_score = score;
      best = &t;
    }
  }
  // A negative best means every target belonged to a different app. Returning
  // one anyway would report another app's traffic under this app's name.
  if (best_score < 0) return nullptr;
  return best;
}

std::string redux_probe_expression(bool include_values) {
  // Walks React's own devtools hook to a component whose props carry
  // something with getState/dispatch/subscribe -- that is a Redux store, and
  // react-redux's Provider is where it is passed. Read-only: nothing is
  // assigned, wrapped or dispatched.
  //
  // Bounded on purpose. This runs inside the app's runtime, and an unbounded
  // walk over a fiber tree would be this tool causing the stall it exists to
  // find.
  std::string expr = R"JS((() => {
  try {
    const hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
    if (!hook || !hook.getFiberRoots) {
      return {found: false, basis: "react devtools hook absent",
              note: "the hook is installed by React in development builds; a "
                    + "release build has none"};
    }
    const ids = Array.from(hook.renderers ? hook.renderers.keys() : []);
    let scanned = 0;
    for (const id of ids) {
      for (const root of Array.from(hook.getFiberRoots(id) || [])) {
        let queue = [root.current];
        while (queue.length && scanned < 4000) {
          const f = queue.shift(); scanned++;
          if (!f) continue;
          const p = f.memoizedProps;
          if (p && p.store && typeof p.store.getState === "function"
                && typeof p.store.dispatch === "function"
                && typeof p.store.subscribe === "function") {
            const st = p.store.getState();
            const keys = (st && typeof st === "object") ? Object.keys(st) : [];
            const out = {found: true, scanned: scanned,
                         basis: "located through React's devtools hook: a "
                                + "component prop exposing getState, dispatch "
                                + "and subscribe",
                         slices: keys.slice(0, 200)};
            __INCLUDE_VALUES__
            return out;
          }
          if (f.child) queue.push(f.child);
          if (f.sibling) queue.push(f.sibling);
        }
      }
    }
    return {found: false, scanned: scanned,
            basis: "walked the fiber tree and found no Redux store",
            note: "the app may not use Redux, or its Provider may sit deeper "
                  + "than the bounded walk reached"};
  } catch (e) {
    return {found: false, basis: "the probe threw", note: String(e)};
  }
})())JS";
  const std::string marker = "__INCLUDE_VALUES__";
  std::string replacement =
      include_values
          // Bounded: a store can be megabytes, and the transport is a
          // debugger socket shared with the app's own traffic.
          ? "try { out.state = JSON.stringify(st).slice(0, 262144); } "
            "catch (e) { out.state_error = String(e); }"
          : "";
  std::size_t at = expr.find(marker);
  if (at != std::string::npos) expr.replace(at, marker.size(), replacement);
  return expr;
}

observe::InspectReport run(const InspectOptions& options,
                           const CancellationToken* cancel) {
  observe::InspectAssembler assembler;
  observe::InspectReport report;

  TargetList list = list_targets(options.metro_port);
  if (!list.metro_reachable || list.targets.empty()) {
    report = assembler.finish(0);
    report.debugger_attached = false;
    observe::SourceStatus s;
    s.name = "react native inspector (via Metro)";
    s.state = observe::SourceState::kUnavailable;
    if (!list.metro_reachable) {
      s.detail = "Metro is not running on 127.0.0.1:" +
                 std::to_string(options.metro_port) + ": " + list.error +
                 ". Nothing below is a statement about the app.";
    } else {
      s.detail = "Metro is running but no app is attached to its inspector. A "
                 "debug build connects itself; a release build runs no "
                 "inspector at all, so there is nothing to attach to. This is "
                 "a missing provider, not an idle app.";
    }
    // Put it first: it is the reason everything else is empty.
    report.sources.insert(report.sources.begin(), s);
    for (observe::SourceStatus& other : report.sources) {
      if (other.state == observe::SourceState::kRanSawNothing) {
        other.state = observe::SourceState::kUnavailable;
        other.detail = "never attached; see the inspector source above";
      }
    }
    return report;
  }

  const InspectorTarget* target = choose_target(list.targets, options.app_id);
  if (target == nullptr) {
    report = assembler.finish(0);
    report.debugger_attached = false;
    observe::SourceStatus s;
    s.name = "react native inspector (via Metro)";
    s.state = observe::SourceState::kUnavailable;
    s.detail = "Metro lists " + std::to_string(list.targets.size()) +
               " target(s), none of them '" + options.app_id +
               "'. Attaching to another app's runtime would report its "
               "traffic under this app's name, so nothing was attached.";
    report.sources.insert(report.sources.begin(), s);
    return report;
  }

  report.app_id = target->app_id;
  report.device_name = target->device_name;
  report.target_title = target->title;

  net::WebSocketClient ws;
  std::string error;
  if (!ws.connect(options.metro_port, target->websocket_path, &error)) {
    report = assembler.finish(0);
    report.app_id = target->app_id;
    report.device_name = target->device_name;
    report.target_title = target->title;
    report.debugger_attached = false;
    observe::SourceStatus s;
    s.name = "react native inspector (via Metro)";
    s.state = observe::SourceState::kRefused;
    s.detail = "the target was listed but the debugger session could not be "
               "opened: " + error +
               ". A session already open elsewhere (React Native DevTools, or "
               "another copy of this tool) holds the only slot.";
    report.sources.insert(report.sources.begin(), s);
    return report;
  }

  int next_id = 1;
  auto command = [&](const std::string& method, const std::string& params) {
    json::Value msg = json::Value::object();
    msg.set("id", json::Value::integer(next_id++));
    msg.set("method", json::Value::string(method));
    if (!params.empty()) {
      json::ParseError perr;
      auto parsed = json::parse(params, &perr);
      if (parsed.has_value()) msg.set("params", *parsed);
    } else {
      msg.set("params", json::Value::object());
    }
    std::string ignored;
    ws.send_text(msg.dump(), &ignored);
    return next_id - 1;
  };

  command("Runtime.enable", {});
  command("Log.enable", {});
  command("Network.enable", {});

  int redux_id = -1;
  if (options.read_redux_state) {
    json::Value params = json::Value::object();
    params.set("expression",
               json::Value::string(redux_probe_expression(
                   options.include_state_values)));
    params.set("returnByValue", json::Value::boolean(true));
    params.set("timeout", json::Value::integer(5000));
    json::Value msg = json::Value::object();
    redux_id = next_id++;
    msg.set("id", json::Value::integer(redux_id));
    msg.set("method", json::Value::string("Runtime.evaluate"));
    msg.set("params", std::move(params));
    std::string ignored;
    ws.send_text(msg.dump(), &ignored);
  }

  observe::StateSnapshot snapshot;
  if (options.read_redux_state) {
    snapshot.note = "the probe was sent but no reply arrived before the "
                    "capture ended";
  } else {
    snapshot.basis = "not read: pass --redux to look for the store";
  }

  const std::int64_t started = now_ms();

  // Taken before the window opens, so its own timestamp places it outside
  // the capture rather than somewhere ambiguous inside it.
  std::vector<observe::Screenshot> shots;
  auto take_shot = [&](observe::ShotMoment moment, const char* suffix) {
    if (!options.screenshots || options.screenshot_device_id.empty()) return;
    observe::ShotOptions so;
    so.platform = options.screenshot_platform;
    so.device_id = options.screenshot_device_id;
    so.out_path = (options.screenshot_dir.empty() ? std::string(".")
                                                  : options.screenshot_dir) +
                  "/screen-" + suffix + ".png";
    so.moment = moment;
    so.capture_start_unix_ms = 0;
    shots.push_back(observe::capture_screen(so));
  };
  take_shot(observe::ShotMoment::kBeforeCapture, "before");

  const std::int64_t deadline = started +
      static_cast<std::int64_t>(options.seconds) * 1000;
  bool disconnected = false;
  std::string disconnect_reason;

  while (now_ms() < deadline) {
    if (cancel != nullptr && cancel->cancelled()) break;
    std::string message;
    int slice = static_cast<int>(deadline - now_ms());
    if (slice <= 0) break;
    net::WsRead rc = ws.read(&message, slice > 500 ? 500 : slice, &error, cancel);
    if (rc == net::WsRead::kTimeout) continue;
    if (rc == net::WsRead::kCancelled) break;
    if (rc == net::WsRead::kClosed || rc == net::WsRead::kError) {
      disconnected = true;
      disconnect_reason = rc == net::WsRead::kClosed
          ? "the app closed the debugger connection (it was reloaded, "
            "backgrounded, or stopped)"
          : error;
      break;
    }
    json::ParseError perr;
    auto doc = json::parse(message, &perr);
    if (!doc.has_value()) continue;

    // A reply to the Redux probe, rather than an event.
    const json::Value* id = doc->find("id");
    if (id != nullptr && id->is_number() &&
        static_cast<int>(id->as_int()) == redux_id) {
      const json::Value* result = doc->find("result");
      const json::Value* inner =
          result != nullptr ? result->find("result") : nullptr;
      const json::Value* value = inner != nullptr ? inner->find("value") : nullptr;
      if (value != nullptr && value->is_object()) {
        const json::Value* found = value->find("found");
        snapshot.found = found != nullptr && found->as_bool();
        const json::Value* basis = value->find("basis");
        if (basis != nullptr && basis->is_string()) snapshot.basis = basis->as_string();
        const json::Value* note = value->find("note");
        snapshot.note = (note != nullptr && note->is_string()) ? note->as_string()
                                                               : std::string();
        const json::Value* scanned = value->find("scanned");
        if (scanned != nullptr && scanned->is_number()) {
          snapshot.fibers_scanned = scanned->as_int();
        }
        const json::Value* slices = value->find("slices");
        if (slices != nullptr && slices->is_array()) {
          for (const json::Value& s : slices->items()) {
            if (s.is_string()) snapshot.slice_names.push_back(s.as_string());
          }
        }
        const json::Value* state = value->find("state");
        if (state != nullptr && state->is_string()) {
          snapshot.state_json = state->as_string();
        }
        const json::Value* serr = value->find("state_error");
        if (serr != nullptr && serr->is_string()) {
          snapshot.note += (snapshot.note.empty() ? "" : " ");
          snapshot.note += "the state itself could not be serialised: " +
                           serr->as_string();
        }
      } else {
        const json::Value* err = doc->find("error");
        snapshot.basis = "the runtime refused the probe";
        snapshot.note = err != nullptr ? err->dump() : "no result returned";
      }
      continue;
    }
    assembler.feed(*doc);
  }

  const std::int64_t elapsed = now_ms() - started;
  take_shot(observe::ShotMoment::kAfterCapture, "after");
  ws.close();

  report.app_id = target->app_id;
  report.device_name = target->device_name;
  report.target_title = target->title;
  observe::InspectReport assembled = assembler.finish(elapsed);
  assembled.app_id = report.app_id;
  assembled.device_name = report.device_name;
  assembled.target_title = report.target_title;
  assembled.state = std::move(snapshot);
  assembled.screenshots = std::move(shots);
  for (const observe::Screenshot& shot : assembled.screenshots) {
    if (shot.captured) continue;
    // A failed screenshot is reported, not dropped: a report with no image
    // and no explanation reads as a report where nobody asked for one.
    assembled.caveats.push_back("A screenshot was requested and not taken: " +
                                shot.error);
  }

  observe::SourceStatus s;
  s.name = "react native inspector (via Metro)";
  s.state = observe::SourceState::kAttached;
  s.detail = "attached to '" + target->title + "' (" + target->description +
             ") with nothing added to the app";
  s.events = 1;
  assembled.sources.insert(assembled.sources.begin(), s);

  if (disconnected) {
    // A capture that ended early is partial, and partial is not quiet.
    assembled.caveats.insert(
        assembled.caveats.begin(),
        "The capture ended early: " + disconnect_reason +
            ". It ran for " + std::to_string(elapsed) + "ms of the " +
            std::to_string(options.seconds * 1000) +
            "ms asked for, so the absence of anything below is not evidence "
            "that it did not happen.");
  }
  if (assembler.unrecognised() > 0) {
    assembled.caveats.push_back(
        std::to_string(assembler.unrecognised()) +
        " protocol message(s) were not recognised and were counted rather "
        "than guessed at.");
  }
  return assembled;
}

}  // namespace mpi::rn
