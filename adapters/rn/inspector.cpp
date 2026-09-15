#include "adapters/rn/inspector.hpp"

#include <chrono>
#include <map>

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
    // `?device=<hash>&page=N`: the hash is the device, the page is a target
    // on it. Names are not unique, so this is the identity to group by.
    const std::size_t at = target.websocket_path.find("device=");
    if (at != std::string::npos) {
      const std::size_t start = at + 7;
      std::size_t end = target.websocket_path.find('&', start);
      if (end == std::string::npos) end = target.websocket_path.size();
      target.device_key = target.websocket_path.substr(start, end - start);
    }
    out.targets.push_back(std::move(target));
  }
  out.metro_answered_empty = out.targets.empty();
  return out;
}

std::string metro_device_hint(const std::string& explicit_hint,
                              const std::string& device_id,
                              const std::string& device_display_name) {
  // Naming a Metro target is answering the question directly.
  if (!explicit_hint.empty()) return explicit_hint;
  // A name discovery holds for this id is what Metro publishes for it.
  if (!device_display_name.empty()) return device_display_name;
  // Otherwise keep what was typed, so a failure quotes it back.
  return device_id;
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
      // Name or key: the key is what distinguishes two devices that share a
      // name, and it is what the ambiguity listing shows for them.
      if (lower(t->device_name).find(want) != std::string::npos ||
          (!t->device_key.empty() &&
           lower(t->device_key).rfind(want, 0) == 0)) {
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

  // Distinct devices among what is left, keyed by Metro's device id and NOT
  // by name. Several targets on *one* device is normal -- a runtime
  // connection plus its auxiliary pages -- and is resolved by preference
  // below. Several *devices* is a question only the caller can answer.
  //
  // Grouping by name was wrong: two simulators of the same model on
  // different runtimes both report `iPad (A16)`, so they would have merged
  // into one group and one of them been chosen silently.
  std::vector<std::string> keys;
  std::vector<std::string> labels;
  for (const InspectorTarget* t : candidates) {
    const std::string key = t->device_key.empty() ? t->device_name
                                                  : t->device_key;
    bool seen = false;
    for (const std::string& k : keys) {
      if (k == key) { seen = true; break; }
    }
    if (seen) continue;
    keys.push_back(key);
    labels.push_back(t->device_name);
  }
  // Where two devices share a name, show enough of the key to tell them
  // apart -- otherwise the list would offer the same string twice.
  for (std::size_t i = 0; i < labels.size(); i++) {
    bool duplicated = false;
    for (std::size_t j = 0; j < labels.size(); j++) {
      if (i != j && labels[j] == labels[i]) { duplicated = true; break; }
    }
    if (duplicated && !keys[i].empty()) {
      labels[i] += " (" + keys[i].substr(0, 8) + ")";
    }
  }
  if (keys.size() > 1) {
    choice.ambiguous = true;
    choice.device_names = labels;
    choice.error = "'" + wanted_app_id + "' is attached from " +
                   std::to_string(keys.size()) +
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

namespace {

/// The fiber walk, shared by every Redux expression.
///
/// One copy, because two copies drift: the one-shot read and the watcher must
/// locate the *same* store, or they would report on different objects and the
/// slice names would not line up with the changes.
const char* kFindStoreJs = R"JS(
  const __mpiFindStore = () => {
    const hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
    if (!hook || !hook.getFiberRoots) {
      return {store: null, scanned: 0,
              basis: "react devtools hook absent",
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
            return {store: p.store, scanned: scanned,
                    basis: "located through React's devtools hook: a component "
                           + "prop exposing getState, dispatch and subscribe"};
          }
          if (f.child) queue.push(f.child);
          if (f.sibling) queue.push(f.sibling);
        }
      }
    }
    return {store: null, scanned: scanned,
            basis: "walked the fiber tree and found no Redux store",
            note: "the app may not use Redux, or its Provider may sit deeper "
                  + "than the bounded walk reached"};
  };
)JS";

/// Undoes whatever a previous install left behind.
///
/// Separate because both install and uninstall need it: install must not
/// stack a second wrapper on a first, and a run whose socket died leaves one
/// installed with nobody draining it.
const char* kTeardownJs = R"JS(
  const __mpiTeardown = () => {
    const w = globalThis.__MPI_REDUX_WATCH__;
    if (!w) return {removed: false};
    const result = {removed: true, dropped: w.dropped || 0};
    try { if (w.unsubscribe) w.unsubscribe(); }
    catch (e) { result.unsubscribe_error = String(e); }
    // Only restore if nothing else wrapped dispatch after us. Overwriting a
    // later wrapper with our saved original would remove someone else's
    // instrumentation -- a worse outcome than leaving ours in place.
    try {
      if (w.store && w.original_dispatch) {
        if (w.store.dispatch === w.wrapped_dispatch) {
          w.store.dispatch = w.original_dispatch;
        } else {
          result.restore_error = "dispatch was replaced again after this "
            + "wrapper was installed, so the original was left alone rather "
            + "than overwriting the newer one";
        }
      }
    } catch (e) { result.restore_error = String(e); }
    try { delete globalThis.__MPI_REDUX_WATCH__; } catch (e) { /* frozen */ }
    return result;
  };
)JS";

}  // namespace

std::string redux_watch_install_expression(const ReduxWatchOptions& options) {
  std::string js = "(() => {\n  try {\n";
  js += kFindStoreJs;
  js += kTeardownJs;
  js += R"JS(
    const priorTeardown = __mpiTeardown();
    const found = __mpiFindStore();
    if (!found.store) {
      return {installed: false, scanned: found.scanned, basis: found.basis,
              note: found.note};
    }
    const store = found.store;
    const cap = __MPI_BUFFER__;
    const withValues = __MPI_VALUES__;

    // Bounded stringify. A slice can be megabytes and the transport is a
    // debugger socket shared with the app's own traffic; a slice too large is
    // reported as too large rather than truncated into invalid JSON, because
    // the C++ side parses this.
    const dump = (v) => {
      try {
        const s = JSON.stringify(v);
        if (s === undefined) return {omitted: "not serialisable"};
        if (s.length > 65536) return {omitted: "larger than 64 KiB"};
        return {json: s};
      } catch (e) { return {omitted: String(e)}; }
    };

    const state = {
      buffer: [], seq: 0, dropped: 0, store: store,
      pending: null, prev: store.getState(),
    };

    const push = (rec) => {
      state.seq++;
      rec.seq = state.seq;
      rec.at = Date.now();
      if (state.buffer.length >= cap) { state.buffer.shift(); state.dropped++; }
      state.buffer.push(rec);
    };

    state.unsubscribe = store.subscribe(() => {
      try {
        const next = store.getState();
        const prev = state.prev;
        state.prev = next;
        // Reference comparison of top-level keys. This is Redux's own
        // contract -- a reducer that did not touch a slice returns the same
        // object -- and it is what react-redux relies on. An app whose
        // reducers mutate state in place breaks the contract and its changes
        // are invisible here; that is reported, not hidden.
        const changed = [];
        if (prev && next && typeof prev === "object"
            && typeof next === "object") {
          for (const k of Object.keys(next)) {
            if (prev[k] !== next[k]) changed.push(k);
          }
          for (const k of Object.keys(prev)) {
            if (!(k in next)) changed.push(k);
          }
        } else if (prev !== next) {
          changed.push("(whole state)");
        }
        const rec = {changed: changed.slice(0, 64)};
        if (changed.length > 64) rec.truncated = "more than 64 slices changed";
        const action = state.pending;
        state.pending = null;
        if (action) {
          rec.how = "dispatch";
          rec.action_type = typeof action.type === "string"
            ? action.type
            : String(action.type);
          if (withValues) {
            const copy = {};
            for (const k of Object.keys(action)) {
              if (k !== "type") copy[k] = action[k];
            }
            const p = dump(copy);
            if (p.json !== undefined) rec.payload = p.json;
            else rec.payload_omitted = p.omitted;
          }
        } else {
          // Wrapped, yet no action arrived: this change came from a dispatch
          // reference that does not go through `store.dispatch`. A thunk's
          // injected dispatch is the common one -- RTK hands it to thunks
          // from the middleware chain, captured when the store was created.
          rec.how = state.wrapped_dispatch ? "bypassed" : "subscribe";
        }
        if (withValues && changed.length) {
          // Only the slices that changed. Sending both whole states per
          // dispatch would put the app's entire store on the wire many times
          // a second.
          const b = {}, a = {};
          for (const k of changed) {
            if (k === "(whole state)") continue;
            b[k] = prev ? prev[k] : undefined;
            a[k] = next ? next[k] : undefined;
          }
          const bd = dump(b), ad = dump(a);
          if (bd.json !== undefined && ad.json !== undefined) {
            rec.before = bd.json;
            rec.after = ad.json;
          } else {
            rec.values_omitted = bd.omitted || ad.omitted;
          }
        }
        push(rec);
      } catch (e) {
        // A throwing listener must not break the app's own dispatch.
        try { push({how: "subscribe", changed: [],
                    truncated: "the listener threw: " + String(e)}); }
        catch (e2) { /* nothing left to do */ }
      }
    });

    __MPI_WRAP_DISPATCH__

    globalThis.__MPI_REDUX_WATCH__ = state;
    const st = store.getState();
    const out = {installed: true, scanned: found.scanned, basis: found.basis,
                 slices: (st && typeof st === "object")
                           ? Object.keys(st).slice(0, 200) : [],
                 dispatch_wrapped: !!state.wrapped_dispatch};
    if (priorTeardown.removed) {
      out.replaced_prior = true;
      if (priorTeardown.restore_error) {
        out.prior_restore_error = priorTeardown.restore_error;
      }
    }
    if (withValues) {
      const d = dump(st);
      if (d.json !== undefined) out.state = d.json;
      else out.state_omitted = d.omitted;
    }
    return out;
  } catch (e) {
    return {installed: false, basis: "the probe threw", note: String(e)};
  }
})())JS";

  // The wrapper is spliced in rather than branched at runtime, so an app that
  // did not ask for it never has the code near its dispatch at all.
  const std::string wrap = options.wrap_dispatch ? R"JS(
    // Records the action, then calls through. The wrapper runs before the
    // reducers, so the subscription that fires next is the one this action
    // caused -- that ordering is what lets a type be attached to a change.
    state.original_dispatch = store.dispatch;
    state.wrapped_dispatch = function (action) {
      try {
        if (action && typeof action === "object") state.pending = action;
      } catch (e) { /* recording must never break a dispatch */ }
      return state.original_dispatch.apply(store, arguments);
    };
    store.dispatch = state.wrapped_dispatch;
)JS"
                                                : "";
  const std::string marker = "__MPI_WRAP_DISPATCH__";
  const std::size_t at = js.find(marker);
  if (at != std::string::npos) js.replace(at, marker.size(), wrap);

  const std::string bmarker = "__MPI_BUFFER__";
  const std::size_t bat = js.find(bmarker);
  if (bat != std::string::npos) {
    // Clamped here rather than trusted: this number becomes an allocation
    // bound inside someone else's app.
    int cap = options.buffer;
    if (cap < 1) cap = 1;
    if (cap > 2000) cap = 2000;
    js.replace(bat, bmarker.size(), std::to_string(cap));
  }
  const std::string vmarker = "__MPI_VALUES__";
  const std::size_t vat = js.find(vmarker);
  if (vat != std::string::npos) {
    js.replace(vat, vmarker.size(), options.include_values ? "true" : "false");
  }
  return js;
}

std::string redux_watch_drain_expression() {
  return R"JS((() => {
  try {
    const w = globalThis.__MPI_REDUX_WATCH__;
    if (!w) {
      return {watching: false,
              note: "no watcher is installed -- the app reloaded, or install "
                    + "never ran"};
    }
    const records = w.buffer;
    const dropped = w.dropped;
    w.buffer = [];
    w.dropped = 0;
    return {watching: true, records: records, dropped: dropped};
  } catch (e) {
    return {watching: false, note: "the drain threw: " + String(e)};
  }
})())JS";
}

std::string redux_watch_uninstall_expression() {
  std::string js = "(() => {\n  try {\n";
  js += kTeardownJs;
  js += R"JS(
    const w = globalThis.__MPI_REDUX_WATCH__;
    const leftover = w ? (w.buffer || []) : [];
    const dropped = w ? (w.dropped || 0) : 0;
    const t = __mpiTeardown();
    // The last records are handed back with the teardown: a capture that
    // stops right after a dispatch would otherwise lose it.
    return {removed: t.removed, records: leftover, dropped: dropped,
            restore_error: t.restore_error || "",
            unsubscribe_error: t.unsubscribe_error || ""};
  } catch (e) {
    return {removed: false, restore_error: String(e)};
  }
})())JS";
  return js;
}

namespace {
/// How often the in-app buffer is emptied. Fast enough that a tap's actions
/// appear while the finger is still on the screen; slow enough that a quiet
/// app is not paying for an evaluate four times a second.
constexpr std::int64_t kReduxDrainIntervalMs = 400;
}  // namespace

/// Applies the watcher's install reply.
///
/// Shared by the one-shot read and the stream. Two copies of this drifted
/// once already -- the reason the fiber walk itself is a single string -- and
/// this one decides what a whole report claims about the store.
void apply_redux_install_reply(const json::Value& value,
                               observe::ReduxObservation* out,
                               observe::StateSnapshot* snap) {
    const json::Value* installed = value.find("installed");
    out->store_found = installed != nullptr && installed->as_bool();
    snap->found = out->store_found;
    if (const json::Value* b = value.find("basis");
        b != nullptr && b->is_string()) {
      out->basis = b->as_string();
      snap->basis = b->as_string();
    }
    if (const json::Value* n = value.find("note");
        n != nullptr && n->is_string()) {
      out->note = n->as_string();
      snap->note = n->as_string();
    } else {
      snap->note.clear();
    }
    if (const json::Value* sc = value.find("scanned");
        sc != nullptr && sc->is_number()) {
      out->fibers_scanned = sc->as_int();
      snap->fibers_scanned = sc->as_int();
    }
    out->slice_names.clear();
    snap->slice_names.clear();
    if (const json::Value* sl = value.find("slices");
        sl != nullptr && sl->is_array()) {
      for (const json::Value& one : sl->items()) {
        if (!one.is_string()) continue;
        out->slice_names.push_back(one.as_string());
        snap->slice_names.push_back(one.as_string());
      }
    }
    if (const json::Value* st = value.find("state");
        st != nullptr && st->is_string()) {
      out->initial_state_json = st->as_string();
      snap->state_json = st->as_string();
    }
    if (const json::Value* w = value.find("dispatch_wrapped");
        w != nullptr) {
      out->dispatch_wrapped = w->as_bool();
    }
    // A previous run that died left its wrapper behind and this one undid
    // it. Saying so matters: it means an earlier capture's numbers came
    // from an app that was still instrumented.
    if (const json::Value* prior = value.find("replaced_prior");
        prior != nullptr && prior->as_bool()) {
      const std::string extra =
          "a watcher from an earlier run was still installed and was "
          "removed first";
      out->note = out->note.empty() ? extra : out->note + "; " + extra;
    }
    if (const json::Value* pe = value.find("prior_restore_error");
        pe != nullptr && pe->is_string() && !pe->as_string().empty()) {
      out->restore_error = pe->as_string();
    }
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
  assembler.capture_detail(options.capture_detail);

  // Command id -> the request whose body it asks for, and how many finished
  // requests have been asked about so far.
  std::map<int, std::string> body_requests;
  std::size_t bodies_asked = 0;
  auto ask_for_bodies = [&] {
    if (!options.capture_detail) return;
    const auto& finished = assembler.finished_requests();
    while (bodies_asked < finished.size()) {
      const std::string& rid = finished[bodies_asked++];
      json::Value params = json::Value::object();
      params.set("requestId", json::Value::string(rid));
      json::Value msg = json::Value::object();
      const int id = next_id++;
      msg.set("id", json::Value::integer(id));
      msg.set("method", json::Value::string("Network.getResponseBody"));
      msg.set("params", std::move(params));
      body_requests[id] = rid;
      std::string ignored;
      ws.send_text(msg.dump(), &ignored);
    }
  };

  int redux_id = -1;
  int redux_install_id = -1;
  int redux_drain_id = -1;
  std::int64_t redux_drained_ms = now_ms();
  observe::ReduxObservation redux;
  // Evaluates an expression and returns the id its reply will carry.
  auto evaluate = [&](const std::string& expression, int timeout_ms) {
    json::Value params = json::Value::object();
    params.set("expression", json::Value::string(expression));
    params.set("returnByValue", json::Value::boolean(true));
    params.set("timeout", json::Value::integer(timeout_ms));
    json::Value msg = json::Value::object();
    const int msg_id = next_id++;
    msg.set("id", json::Value::integer(msg_id));
    msg.set("method", json::Value::string("Runtime.evaluate"));
    msg.set("params", std::move(params));
    std::string ignored;
    ws.send_text(msg.dump(), &ignored);
    return msg_id;
  };

  if (options.watch_redux) {
    // Watching supersedes the one-shot read: install reports the same store
    // location, slice names and initial state.
    ReduxWatchOptions wo = options.redux_watch;
    wo.include_values = options.include_state_values;
    redux_install_id = evaluate(redux_watch_install_expression(wo), 5000);
  } else if (options.read_redux_state) {
    redux_id = evaluate(redux_probe_expression(options.include_state_values),
                        5000);
  }

  observe::StateSnapshot snapshot;
  if (options.watch_redux) {
    snapshot.basis = "watching the store; no install reply arrived before the "
                     "capture ended";
    redux.basis = snapshot.basis;
  } else if (options.read_redux_state) {
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

    // A reply to a body request.
    const json::Value* reply_id = doc->find("id");
    if (reply_id != nullptr && reply_id->is_number()) {
      const auto it = body_requests.find(static_cast<int>(reply_id->as_int()));
      if (it != body_requests.end()) {
        const std::string rid = it->second;
        body_requests.erase(it);
        const json::Value* err = doc->find("error");
        if (err != nullptr) {
          const json::Value* m = err->find("message");
          assembler.set_response_body_unavailable(
              rid, m != nullptr && m->is_string()
                       ? m->as_string()
                       : std::string("the runtime returned no body"));
        } else {
          const json::Value* result = doc->find("result");
          const json::Value* body =
              result != nullptr ? result->find("body") : nullptr;
          const json::Value* b64 =
              result != nullptr ? result->find("base64Encoded") : nullptr;
          if (body != nullptr && body->is_string()) {
            assembler.set_response_body(rid, body->as_string(),
                                        b64 != nullptr && b64->as_bool());
          } else {
            assembler.set_response_body_unavailable(
                rid, "the runtime returned a reply with no body field");
          }
        }
        continue;
      }
    }

    // A reply to the Redux probe, rather than an event.
    const json::Value* id = doc->find("id");
    if (id != nullptr && id->is_number() &&
        (static_cast<int>(id->as_int()) == redux_install_id ||
         static_cast<int>(id->as_int()) == redux_drain_id)) {
      const bool was_install =
          static_cast<int>(id->as_int()) == redux_install_id;
      if (!was_install) redux_drain_id = -1;
      const json::Value* result = doc->find("result");
      const json::Value* inner =
          result != nullptr ? result->find("result") : nullptr;
      const json::Value* value =
          inner != nullptr ? inner->find("value") : nullptr;
      if (value == nullptr || !value->is_object()) {
        const json::Value* err = doc->find("error");
        if (was_install) {
          redux.basis = "the runtime refused the watcher";
          redux.note = err != nullptr ? err->dump() : "no result";
          snapshot.basis = redux.basis;
          snapshot.note = redux.note;
        }
        continue;
      }
      if (was_install) {
        apply_redux_install_reply(*value, &redux, &snapshot);
      } else {
        observe::ingest_redux_drain(*value, options.include_state_values,
                                    &redux);
      }
      continue;
    }
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
    ask_for_bodies();
  // Empty the in-app buffer as the window runs, so a long capture is not
  // relying on a buffer sized for the whole of it.
  if (options.watch_redux && redux_drain_id < 0 &&
      now_ms() - redux_drained_ms >= kReduxDrainIntervalMs) {
    redux_drained_ms = now_ms();
    redux_drain_id = evaluate(redux_watch_drain_expression(), 3000);
  }
  }

  const std::int64_t elapsed = now_ms() - started;
  take_shot(observe::ShotMoment::kAfterCapture, "after");

  // Put the app back, and collect the records since the last drain. A
  // wrapper left installed keeps recording into a buffer nobody drains.
  if (options.watch_redux && redux_install_id >= 0) {
    const int uninstall_id =
        evaluate(redux_watch_uninstall_expression(), 3000);
    const std::int64_t restore_deadline = now_ms() + 1500;
    bool answered = false;
    while (now_ms() < restore_deadline && !answered) {
      std::string tail_message;
      std::string tail_error;
      const net::WsRead rc =
          ws.read(&tail_message, 100, &tail_error, nullptr);
      if (rc == net::WsRead::kClosed || rc == net::WsRead::kError) break;
      if (rc != net::WsRead::kMessage) continue;
      json::ParseError perr;
      auto doc = json::parse(tail_message, &perr);
      if (!doc.has_value()) continue;
      const json::Value* rid = doc->find("id");
      if (rid == nullptr || !rid->is_number() ||
          static_cast<int>(rid->as_int()) != uninstall_id) {
        // Still the app's own traffic, and it is still real. A capture must
        // not discard its last events just because it is shutting down.
        assembler.feed(*doc);
        continue;
      }
      answered = true;
      const json::Value* result = doc->find("result");
      const json::Value* inner =
          result != nullptr ? result->find("result") : nullptr;
      const json::Value* value =
          inner != nullptr ? inner->find("value") : nullptr;
      if (value != nullptr && value->is_object()) {
        observe::ingest_redux_drain(*value, options.include_state_values,
                                    &redux);
        if (const json::Value* re = value->find("restore_error");
            re != nullptr && re->is_string() && !re->as_string().empty()) {
          redux.restore_error = re->as_string();
        }
      }
    }
    if (!answered && redux.restore_error.empty()) {
      redux.restore_error = disconnected
          ? "the connection had already ended, so the watcher could not be "
            "removed; it goes when the app next reloads, and the next capture "
            "removes it before installing its own"
          : "the app did not confirm within 1.5s that the watcher was "
            "removed; it goes when the app next reloads, and the next capture "
            "removes it before installing its own";
    }
  }
  ws.close();

  report.app_id = target->app_id;
  report.device_name = target->device_name;
  report.target_title = target->title;
  observe::InspectReport assembled = assembler.finish(elapsed);
  assembled.app_id = report.app_id;
  assembled.device_name = report.device_name;
  assembled.target_title = report.target_title;
  assembled.state = std::move(snapshot);
  assembled.redux = std::move(redux);
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
  /// Command id -> the request whose body it asks for.
  std::map<int, std::string> body_requests;
  /// How many finished requests have already been asked about, so each is
  /// asked once.
  std::size_t bodies_asked = 0;
  InspectOptions options;
  InspectorTarget target;
  std::int64_t started_ms = 0;
  int next_id = 1;
  int redux_id = -1;
  /// The watcher's install reply, and the drain currently in flight.
  int redux_install_id = -1;
  int redux_drain_id = -1;
  int redux_uninstall_id = -1;
  /// When the last drain was sent. Drains are paced rather than sent on every
  /// pump: the buffer is in the app, so polling faster only adds evaluate
  /// round-trips to a socket the app is also using.
  std::int64_t redux_drained_ms = 0;
  observe::StateSnapshot snapshot;
  observe::ReduxObservation redux;
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
  impl_->assembler.capture_detail(options.capture_detail);

  if (options.watch_redux) {
    // Watching supersedes the one-shot read: install returns the same store
    // location, slice names and initial state, so sending both would walk
    // the fiber tree twice for one answer.
    ReduxWatchOptions wo = options.redux_watch;
    wo.include_values = options.include_state_values;
    json::Value params = json::Value::object();
    params.set("expression",
               json::Value::string(redux_watch_install_expression(wo)));
    params.set("returnByValue", json::Value::boolean(true));
    params.set("timeout", json::Value::integer(5000));
    json::Value msg = json::Value::object();
    impl_->redux_install_id = impl_->next_id++;
    msg.set("id", json::Value::integer(impl_->redux_install_id));
    msg.set("method", json::Value::string("Runtime.evaluate"));
    msg.set("params", std::move(params));
    std::string ignored;
    impl_->ws.send_text(msg.dump(), &ignored);
    impl_->snapshot.basis = "watching the store; the install reply has not "
                            "arrived yet";
    impl_->redux.basis = impl_->snapshot.basis;
    impl_->redux_drained_ms = now_ms();
  } else if (options.read_redux_state) {
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

/// Sends `Network.getResponseBody` for any finished request not yet asked
/// about.
///
/// Done here rather than in the assembler because the assembler talks to
/// nothing: it records what arrives, and issuing a command is the session's
/// job. One request per body, and each asked exactly once.
void InspectStream::request_pending_bodies() {
  if (!impl_->options.capture_detail) return;
  const auto& finished = impl_->assembler.finished_requests();
  while (impl_->bodies_asked < finished.size()) {
    const std::string& rid = finished[impl_->bodies_asked++];
    json::Value params = json::Value::object();
    params.set("requestId", json::Value::string(rid));
    json::Value msg = json::Value::object();
    const int id = impl_->next_id++;
    msg.set("id", json::Value::integer(id));
    msg.set("method", json::Value::string("Network.getResponseBody"));
    msg.set("params", std::move(params));
    impl_->body_requests[id] = rid;
    std::string ignored;
    impl_->ws.send_text(msg.dump(), &ignored);
  }
}

void InspectStream::handle(const json::Value& message) {
  const json::Value* id = message.find("id");
  if (id != nullptr && id->is_number()) {
    const auto it = impl_->body_requests.find(static_cast<int>(id->as_int()));
    if (it != impl_->body_requests.end()) {
      const std::string rid = it->second;
      impl_->body_requests.erase(it);
      const json::Value* err = message.find("error");
      if (err != nullptr) {
        // A HEAD or a 204 genuinely has no body, and the runtime says so with
        // this same error. Recorded as "none to give" rather than as a
        // failure, because they are different facts.
        const json::Value* m = err->find("message");
        impl_->assembler.set_response_body_unavailable(
            rid, m != nullptr && m->is_string()
                     ? m->as_string()
                     : std::string("the runtime returned no body"));
      } else {
        const json::Value* result = message.find("result");
        const json::Value* body =
            result != nullptr ? result->find("body") : nullptr;
        const json::Value* b64 =
            result != nullptr ? result->find("base64Encoded") : nullptr;
        if (body != nullptr && body->is_string()) {
          impl_->assembler.set_response_body(
              rid, body->as_string(), b64 != nullptr && b64->as_bool());
        } else {
          impl_->assembler.set_response_body_unavailable(
              rid, "the runtime returned a reply with no body field");
        }
      }
      return;
    }
  }
  // The watcher's install reply, and each drain's.
  if (id != nullptr && id->is_number() &&
      (static_cast<int>(id->as_int()) == impl_->redux_install_id ||
       static_cast<int>(id->as_int()) == impl_->redux_drain_id ||
       static_cast<int>(id->as_int()) == impl_->redux_uninstall_id)) {
    const int which = static_cast<int>(id->as_int());
    const bool is_install = which == impl_->redux_install_id;
    const bool is_drain = which == impl_->redux_drain_id;
    if (is_drain) impl_->redux_drain_id = -1;
    const json::Value* result = message.find("result");
    const json::Value* inner =
        result != nullptr ? result->find("result") : nullptr;
    const json::Value* value = inner != nullptr ? inner->find("value") : nullptr;
    if (value == nullptr || !value->is_object()) {
      const json::Value* err = message.find("error");
      if (is_install) {
        impl_->redux.basis = "the runtime refused the watcher";
        impl_->redux.note = err != nullptr ? err->dump() : "no result";
        impl_->snapshot.basis = impl_->redux.basis;
        impl_->snapshot.note = impl_->redux.note;
      } else if (impl_->redux.note.empty()) {
        // A refused drain leaves the records already collected intact, so
        // the list is a tail rather than wrong -- which is worth saying,
        // because it stops growing and silence reads as an idle app.
        impl_->redux.note = "a drain was refused by the runtime: " +
                            (err != nullptr ? err->dump() : "no result");
      }
      return;
    }
    if (is_install) {
      apply_redux_install_reply(*value, &impl_->redux, &impl_->snapshot);
      return;
    }
    // A drain, or the final uninstall -- both carry records, and the
    // uninstall's are the ones a capture would otherwise lose between its
    // last drain and stopping.
    observe::ingest_redux_drain(*value, impl_->options.include_state_values,
                                &impl_->redux);
    if (const json::Value* re = value->find("restore_error");
        re != nullptr && re->is_string() && !re->as_string().empty()) {
      impl_->redux.restore_error = re->as_string();
    }
    return;
  }
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
    // A request that just finished can be asked about immediately, so the
    // body arrives in the same poll rather than the next one.
    request_pending_bodies();
    drain_redux();
  }
}

void InspectStream::drain_redux() {
  if (!running_ || impl_->redux_install_id < 0) return;
  // One drain in flight at a time. Sending another while the first is
  // outstanding would interleave two buffers and lose the ordering that makes
  // a list of actions readable.
  if (impl_->redux_drain_id >= 0) return;
  const std::int64_t now = now_ms();
  if (now - impl_->redux_drained_ms < kReduxDrainIntervalMs) return;
  impl_->redux_drained_ms = now;
  json::Value params = json::Value::object();
  params.set("expression", json::Value::string(redux_watch_drain_expression()));
  params.set("returnByValue", json::Value::boolean(true));
  params.set("timeout", json::Value::integer(3000));
  json::Value msg = json::Value::object();
  impl_->redux_drain_id = impl_->next_id++;
  msg.set("id", json::Value::integer(impl_->redux_drain_id));
  msg.set("method", json::Value::string("Runtime.evaluate"));
  msg.set("params", std::move(params));
  std::string ignored;
  impl_->ws.send_text(msg.dump(), &ignored);
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
  report.redux = impl_->redux;
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
  // Put the app back before closing the socket. A wrapper left installed
  // keeps recording into a buffer nobody drains, and on the app's next
  // dispatch the app is still paying for instrumentation whose owner has
  // gone. The reply carries the records since the last drain, so this also
  // recovers the tail of the capture.
  if (running_ && impl_->redux_install_id >= 0) {
    json::Value params = json::Value::object();
    params.set("expression",
               json::Value::string(redux_watch_uninstall_expression()));
    params.set("returnByValue", json::Value::boolean(true));
    params.set("timeout", json::Value::integer(3000));
    json::Value msg = json::Value::object();
    impl_->redux_uninstall_id = impl_->next_id++;
    msg.set("id", json::Value::integer(impl_->redux_uninstall_id));
    msg.set("method", json::Value::string("Runtime.evaluate"));
    msg.set("params", std::move(params));
    std::string ignored;
    if (impl_->ws.send_text(msg.dump(), &ignored)) {
      // Bounded wait for the reply. Worth waiting for -- it is the only
      // confirmation that the app is unmodified again -- but not worth
      // hanging a UI over, so a timeout is recorded rather than retried.
      const std::int64_t deadline = now_ms() + 1500;
      bool answered = false;
      while (now_ms() < deadline && !answered) {
        std::string message;
        std::string error;
        const net::WsRead rc = impl_->ws.read(&message, 100, &error, nullptr);
        if (rc == net::WsRead::kClosed || rc == net::WsRead::kError) break;
        if (rc != net::WsRead::kMessage) continue;
        json::ParseError perr;
        auto doc = json::parse(message, &perr);
        if (!doc.has_value()) continue;
        const json::Value* rid = doc->find("id");
        answered = rid != nullptr && rid->is_number() &&
                   static_cast<int>(rid->as_int()) == impl_->redux_uninstall_id;
        handle(*doc);
      }
      if (!answered && impl_->redux.restore_error.empty()) {
        impl_->redux.restore_error = disconnect_reason_.empty()
            ? "the app did not confirm within 1.5s that the watcher was "
              "removed; it goes when the app next reloads, and the next "
              "capture removes it before installing its own"
            : "the connection had already ended (" + disconnect_reason_ +
                  "), so the watcher could not be removed; it goes when the "
                  "app next reloads, and the next capture removes it before "
                  "installing its own";
      }
    } else if (impl_->redux.restore_error.empty()) {
      impl_->redux.restore_error =
          "the socket would not carry the removal request, so the watcher is "
          "still installed until the app reloads";
    }
    impl_->redux_install_id = -1;
  }

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
