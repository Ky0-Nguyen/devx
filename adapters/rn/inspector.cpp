#include "adapters/rn/inspector.hpp"

#include <chrono>

#include "core/observe/screenshot.hpp"

#include "core/net/loopback_http.hpp"
#include "core/net/websocket_client.hpp"
#include "core/util/json.hpp"

namespace mpi::rn {
namespace {

const std::string kEmpty;

std::string lower(std::string v) {
  for (char& c : v) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return v;
}

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

TargetChoice choose_target(const std::vector<InspectorTarget>& targets,
                           const std::string& wanted_app_id,
                           const std::string& device_hint) {
  TargetChoice choice;

  // Only the app's own targets are candidates. Attaching to another app's
  // runtime would report its traffic under this app's name.
  std::vector<const InspectorTarget*> candidates;
  for (const InspectorTarget& t : targets) {
    if (!wanted_app_id.empty() && t.app_id != wanted_app_id) continue;
    candidates.push_back(&t);
  }
  if (candidates.empty()) {
    for (const InspectorTarget& t : targets) {
      choice.device_names.push_back(t.device_name + " (" + t.app_id + ")");
    }
    choice.error = wanted_app_id.empty()
        ? "Metro listed no attachable target"
        : "no target for '" + wanted_app_id + "'";
    return choice;
  }

  // Narrow by device when asked. Metro publishes only a device *name*, so
  // that is what a hint can match.
  if (!device_hint.empty()) {
    const std::string want = lower(device_hint);
    std::vector<const InspectorTarget*> matched;
    for (const InspectorTarget* t : candidates) {
      if (lower(t->device_name).find(want) != std::string::npos) {
        matched.push_back(t);
      }
    }
    if (matched.empty()) {
      for (const InspectorTarget* t : candidates) {
        choice.device_names.push_back(t->device_name);
      }
      choice.error = "no attached device matches '" + device_hint + "'";
      return choice;
    }
    candidates = std::move(matched);
  }

  // Distinct devices among what is left. Several targets on *one* device is
  // normal -- Metro lists a runtime connection and auxiliary pages -- and is
  // resolved by preference below. Several *devices* is a question only the
  // caller can answer.
  std::vector<std::string> devices;
  for (const InspectorTarget* t : candidates) {
    bool seen = false;
    for (const std::string& d : devices) {
      if (d == t->device_name) { seen = true; break; }
    }
    if (!seen) devices.push_back(t->device_name);
  }
  if (devices.size() > 1) {
    choice.ambiguous = true;
    choice.device_names = devices;
    choice.error = "'" + wanted_app_id + "' is attached from " +
                   std::to_string(devices.size()) +
                   " devices; name one rather than having one chosen";
    return choice;
  }

  // One device. Prefer the full runtime connection: it is the target that
  // carries Runtime and Network, where the auxiliary pages answer far less.
  const InspectorTarget* best = nullptr;
  int best_score = -1;
  for (const InspectorTarget* t : candidates) {
    int score = 0;
    if (t->description.find("C++ connection") != std::string::npos) score += 4;
    if (t->description.find("Bridgeless") != std::string::npos) score += 2;
    // "UI [C++ connection]" is an auxiliary page on the same runtime.
    if (t->description.rfind("UI ", 0) == 0) score -= 3;
    if (score > best_score) {
      best_score = score;
      best = t;
    }
  }
  choice.target = best;
  return choice;
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

  const TargetChoice choice =
      choose_target(list.targets, options.app_id, options.device_hint);
  const InspectorTarget* target = choice.target;
  if (target == nullptr) {
    report = assembler.finish(0);
    report.debugger_attached = false;
    observe::SourceStatus s;
    s.name = "react native inspector (via Metro)";
    s.state = observe::SourceState::kUnavailable;
    std::string detail = choice.error;
    if (choice.ambiguous) {
      // Naming the choices is the point: the operator knows which device
      // they meant, and this tool must not decide for them.
      detail += ". Attached devices:";
      for (const std::string& d : choice.device_names) detail += " [" + d + "]";
      detail += ". Pass the device to observe.";
    } else if (!choice.device_names.empty()) {
      detail += ". What is attached:";
      for (const std::string& d : choice.device_names) detail += " [" + d + "]";
    }
    s.detail = detail;
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


// --- InspectStream ---------------------------------------------------------

struct InspectStream::Impl {
  net::WebSocketClient ws;
  observe::InspectAssembler assembler;
  InspectOptions options;
  InspectorTarget target;
  std::int64_t started_ms = 0;
  int next_id = 1;
  int redux_id = -1;
  observe::StateSnapshot snapshot;
  std::vector<observe::Screenshot> shots;
};

InspectStream::InspectStream() : impl_(std::make_unique<Impl>()) {}
InspectStream::~InspectStream() { stop(); }

bool InspectStream::start(const InspectOptions& options, std::string* error) {
  impl_->options = options;
  const TargetList list = list_targets(options.metro_port);
  if (!list.metro_reachable) {
    *error = "Metro is not running on 127.0.0.1:" +
             std::to_string(options.metro_port) + ": " + list.error +
             ". This says nothing about the app.";
    return false;
  }
  if (list.targets.empty()) {
    *error = "Metro is running but no app is attached to its inspector. A "
             "debug build connects itself; a release build runs no inspector "
             "at all, so there is nothing to attach to. This is a missing "
             "provider, not an idle app.";
    return false;
  }
  const TargetChoice choice =
      choose_target(list.targets, options.app_id, options.device_hint);
  const InspectorTarget* chosen = choice.target;
  if (chosen == nullptr) {
    *error = choice.error;
    if (choice.ambiguous) {
      *error += ". Attached devices:";
      for (const std::string& d : choice.device_names) *error += " [" + d + "]";
      *error += ". Name the device rather than having one chosen for you: "
                "attaching to the wrong one reports its traffic under the "
                "right app's name.";
    } else if (!choice.device_names.empty()) {
      *error += ". What is attached:";
      for (const std::string& d : choice.device_names) *error += " [" + d + "]";
    }
    return false;
  }
  impl_->target = *chosen;
  if (!impl_->ws.connect(options.metro_port, chosen->websocket_path, error)) {
    *error = "the target was listed but the debugger session could not be "
             "opened: " + *error +
             ". A session already open elsewhere (React Native DevTools, or "
             "another copy of this tool) holds the only slot.";
    return false;
  }

  auto command = [&](const std::string& method) {
    json::Value msg = json::Value::object();
    msg.set("id", json::Value::integer(impl_->next_id++));
    msg.set("method", json::Value::string(method));
    msg.set("params", json::Value::object());
    std::string ignored;
    impl_->ws.send_text(msg.dump(), &ignored);
  };
  command("Runtime.enable");
  command("Log.enable");
  command("Network.enable");

  if (options.read_redux_state) {
    json::Value params = json::Value::object();
    params.set("expression", json::Value::string(
        redux_probe_expression(options.include_state_values)));
    params.set("returnByValue", json::Value::boolean(true));
    params.set("timeout", json::Value::integer(5000));
    json::Value msg = json::Value::object();
    impl_->redux_id = impl_->next_id++;
    msg.set("id", json::Value::integer(impl_->redux_id));
    msg.set("method", json::Value::string("Runtime.evaluate"));
    msg.set("params", std::move(params));
    std::string ignored;
    impl_->ws.send_text(msg.dump(), &ignored);
    impl_->snapshot.note = "the probe was sent and no reply has arrived yet";
  } else {
    impl_->snapshot.basis = "not read: enable the Redux option to look for "
                            "the store";
  }

  if (options.screenshots && !options.screenshot_device_id.empty()) {
    observe::ShotOptions so;
    so.platform = options.screenshot_platform;
    so.device_id = options.screenshot_device_id;
    so.out_path = (options.screenshot_dir.empty() ? std::string(".")
                                                  : options.screenshot_dir) +
                  "/screen-before.png";
    so.moment = observe::ShotMoment::kBeforeCapture;
    impl_->shots.push_back(observe::capture_screen(so));
  }

  impl_->started_ms = now_ms();
  running_ = true;
  disconnect_reason_.clear();
  return true;
}

void InspectStream::handle(const json::Value& message) {
  const json::Value* id = message.find("id");
  if (id != nullptr && id->is_number() &&
      static_cast<int>(id->as_int()) == impl_->redux_id) {
    const json::Value* result = message.find("result");
    const json::Value* inner = result != nullptr ? result->find("result") : nullptr;
    const json::Value* value = inner != nullptr ? inner->find("value") : nullptr;
    if (value != nullptr && value->is_object()) {
      const json::Value* found = value->find("found");
      impl_->snapshot.found = found != nullptr && found->as_bool();
      const json::Value* basis = value->find("basis");
      if (basis != nullptr && basis->is_string()) {
        impl_->snapshot.basis = basis->as_string();
      }
      const json::Value* note = value->find("note");
      impl_->snapshot.note = (note != nullptr && note->is_string())
                                 ? note->as_string() : std::string();
      const json::Value* scanned = value->find("scanned");
      if (scanned != nullptr && scanned->is_number()) {
        impl_->snapshot.fibers_scanned = scanned->as_int();
      }
      const json::Value* slices = value->find("slices");
      impl_->snapshot.slice_names.clear();
      if (slices != nullptr && slices->is_array()) {
        for (const json::Value& sl : slices->items()) {
          if (sl.is_string()) impl_->snapshot.slice_names.push_back(sl.as_string());
        }
      }
      const json::Value* state = value->find("state");
      if (state != nullptr && state->is_string()) {
        impl_->snapshot.state_json = state->as_string();
      }
    } else {
      const json::Value* err = message.find("error");
      impl_->snapshot.basis = "the runtime refused the probe";
      impl_->snapshot.note = err != nullptr ? err->dump() : "no result";
    }
    return;
  }
  impl_->assembler.feed(message);
}

void InspectStream::pump(int budget_ms) {
  if (!running_) return;
  const std::int64_t deadline = now_ms() + budget_ms;
  for (;;) {
    const std::int64_t left = deadline - now_ms();
    if (left <= 0) return;
    std::string message;
    std::string error;
    // Short slices, so a quiet app returns promptly instead of holding the
    // caller for the whole budget.
    const int slice = static_cast<int>(left > 100 ? 100 : left);
    const net::WsRead rc = impl_->ws.read(&message, slice, &error, nullptr);
    if (rc == net::WsRead::kTimeout) continue;
    if (rc == net::WsRead::kClosed || rc == net::WsRead::kError) {
      running_ = false;
      disconnect_reason_ = rc == net::WsRead::kClosed
          ? "the app closed the debugger connection (it was reloaded, "
            "backgrounded, or stopped)"
          : error;
      return;
    }
    if (rc != net::WsRead::kMessage) return;
    json::ParseError perr;
    auto doc = json::parse(message, &perr);
    if (doc.has_value()) handle(*doc);
  }
}

observe::InspectReport InspectStream::snapshot() const {
  // The assembler is cumulative, and `finish()` is non-destructive, so this
  // can be called repeatedly while the stream is still open.
  observe::InspectAssembler copy = impl_->assembler;
  observe::InspectReport report = copy.finish(now_ms() - impl_->started_ms);
  report.app_id = impl_->target.app_id;
  report.device_name = impl_->target.device_name;
  report.target_title = impl_->target.title;
  report.state = impl_->snapshot;
  report.screenshots = impl_->shots;

  observe::SourceStatus s;
  s.name = "react native inspector (via Metro)";
  s.state = running_ ? observe::SourceState::kAttached
                     : observe::SourceState::kRefused;
  s.detail = running_
      ? "attached to '" + impl_->target.title + "' (" +
            impl_->target.description + ") with nothing added to the app"
      : "the connection ended: " + disconnect_reason_;
  s.events = 1;
  report.sources.insert(report.sources.begin(), s);

  if (!running_ && !disconnect_reason_.empty()) {
    report.caveats.insert(
        report.caveats.begin(),
        "The observation ended early: " + disconnect_reason_ +
            ". The absence of anything below is not evidence that it did not "
            "happen.");
  }
  return report;
}

void InspectStream::stop() {
  if (impl_ == nullptr) return;
  if (running_ && impl_->options.screenshots &&
      !impl_->options.screenshot_device_id.empty()) {
    observe::ShotOptions so;
    so.platform = impl_->options.screenshot_platform;
    so.device_id = impl_->options.screenshot_device_id;
    so.out_path = (impl_->options.screenshot_dir.empty()
                       ? std::string(".")
                       : impl_->options.screenshot_dir) +
                  "/screen-after.png";
    so.moment = observe::ShotMoment::kAfterCapture;
    so.capture_start_unix_ms = 0;
    impl_->shots.push_back(observe::capture_screen(so));
  }
  impl_->ws.close();
  running_ = false;
}

}  // namespace mpi::rn
