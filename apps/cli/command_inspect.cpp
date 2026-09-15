// `mpi inspect` -- read a running app's network calls, console output and
// Redux state without adding anything to the app.
//
// The request behind this was "Reactotron, but nothing installed". Reactotron
// needs the app to import a client; this needs nothing, because a React
// Native debug build already runs an inspector and already connects itself to
// Metro. See core/observe/inspect.hpp for what that does and does not reach.
//
// A separate command from `record` because it produces a different kind of
// thing. `record` writes a session package of measurements; this writes an
// observation of what the app said and asked for, taken with a debugger
// attached -- which means it is explicitly *not* a performance measurement,
// and keeping the two commands apart keeps that from being blurred.
#include <iomanip>
#include <iostream>

#include "adapters/rn/inspector.hpp"
#include "apps/cli/cli.hpp"

namespace mpi::cli {
namespace {

std::string elide(const std::string& s, std::size_t width) {
  if (s.size() <= width) return s;
  if (width <= 2) return s.substr(0, width);
  // Keep the tail: a URL's path is what identifies it, and cutting the end
  // leaves every request from one host looking identical.
  //
  // ASCII marker rather than an ellipsis character: these strings sit in
  // `setw` columns, `setw` counts bytes, and a three-byte "…" made every
  // padded column two characters short -- the device column ran straight
  // into the one after it.
  return ".." + s.substr(s.size() - (width - 2));
}


/// The name Metro would publish for a device id, or empty when discovery
/// cannot say.
///
/// Only runs when there is an id to translate and no explicit Metro target,
/// because it costs a discovery pass. A failure here is not fatal: the id is
/// then used as the hint and the mismatch is reported against what was typed.
std::string metro_name_for_device(const Invocation& inv,
                                  const std::string& device_id) {
  if (device_id.empty() || inv.has_flag("target-device")) return "";
  // Through the CLI's own factory, which is what registers the providers and
  // honours --no-simulators. A bare DiscoveryService has none registered, so
  // it returns an empty snapshot and every id silently fails to resolve.
  auto svc = make_discovery(inv.global);
  const auto snap = svc.snapshot(provider_options(inv.global),
                                 /*include_apps=*/false);
  for (const auto& d : snap.devices) {
    if (d.matches_id(device_id)) return d.display_name;
  }
  return "";
}

}  // namespace

ExitCode cmd_inspect(const Invocation& inv) {
  rn::InspectOptions opts;
  opts.app_id = inv.global.app;
  // Metro publishes a device *name* and nothing else -- no adb serial, no
  // simulator UDID -- so that is what a hint can match. `--device` carries an
  // id, so it is translated to the name discovery holds for that id rather
  // than passed through; see rn::metro_device_hint for why that mattered.
  // `--target-device` is the way to name a Metro target directly.
  opts.device_hint = rn::metro_device_hint(
      inv.flag("target-device"), inv.global.device,
      metro_name_for_device(inv, inv.global.device));
  opts.read_redux_state = inv.has_flag("redux");
  opts.include_state_values = inv.has_flag("redux-values");
  if (opts.include_state_values) opts.read_redux_state = true;
  // Watching the store, rather than reading it once. `--redux-actions` also
  // wraps `dispatch`, which is the only thing here that modifies the running
  // app -- so it is its own flag and not implied by anything else.
  opts.watch_redux = inv.has_flag("redux-watch") ||
                     inv.has_flag("redux-actions");
  opts.redux_watch.wrap_dispatch = inv.has_flag("redux-actions");
  if (opts.watch_redux) opts.read_redux_state = true;
  if (inv.has_flag("redux-buffer")) {
    const int cap = std::atoi(inv.flag("redux-buffer").c_str());
    if (cap <= 0) {
      std::cerr << "error: --redux-buffer must be a positive count\n";
      return ExitCode::kUsage;
    }
    opts.redux_watch.buffer = cap;
  }

  if (inv.has_flag("metro-port")) {
    const int port = std::atoi(inv.flag("metro-port").c_str());
    if (port <= 0 || port > 65535) {
      std::cerr << "error: --metro-port must be a port number\n";
      return ExitCode::kUsage;
    }
    opts.metro_port = static_cast<std::uint16_t>(port);
  }
  // Headers and bodies: what makes a request inspectable rather than listed.
  // Off unless asked, because this is where the bearer tokens are.
  opts.capture_detail = inv.has_flag("detail");
  opts.screenshots = inv.has_flag("screenshot");
  if (opts.screenshots) {
    if (inv.global.device.empty()) {
      // Metro knows the app, not the adb serial. Guessing one would
      // photograph whichever device answered first.
      std::cerr << "error: --screenshot needs --device: Metro's inspector "
                   "knows the app but not\n       which device it is on, and "
                   "photographing the wrong one is worse than\n       "
                   "photographing none\n";
      return ExitCode::kUsage;
    }
    opts.screenshot_device_id = inv.global.device;
    opts.screenshot_platform = inv.global.platform == "ios"
                                   ? model::Platform::kIos
                                   : model::Platform::kAndroid;
    opts.screenshot_dir = inv.flag("screenshot-dir", ".");
  }

  if (inv.has_flag("seconds")) {
    const int secs = std::atoi(inv.flag("seconds").c_str());
    if (secs <= 0 || secs > 3600) {
      std::cerr << "error: --seconds must be between 1 and 3600\n";
      return ExitCode::kUsage;
    }
    opts.seconds = secs;
  }

  if (inv.has_flag("targets")) {
    // Listing is separate from attaching because "what is attachable" is the
    // first question when nothing appears, and answering it must not require
    // holding the single debugger slot for fifteen seconds.
    const rn::TargetList list = rn::list_targets(opts.metro_port);
    if (inv.global.json) {
      json::Value out = json::Value::object();
      out.set("metro_reachable", json::Value::boolean(list.metro_reachable));
      out.set("metro_port", json::Value::integer(opts.metro_port));
      if (!list.error.empty()) out.set("error", json::Value::string(list.error));
      json::Value arr = json::Value::array();
      for (const auto& t : list.targets) {
        json::Value v = json::Value::object();
        v.set("app_id", json::Value::string(t.app_id));
        v.set("title", json::Value::string(t.title));
        v.set("description", json::Value::string(t.description));
        v.set("device_name", json::Value::string(t.device_name));
        arr.push_back(std::move(v));
      }
      out.set("targets", std::move(arr));
      print_json(out);
      return list.metro_reachable ? ExitCode::kOk : ExitCode::kCollectionError;
    }
    if (!list.metro_reachable) {
      std::cerr << "Metro is not answering on 127.0.0.1:" << opts.metro_port
                << ": " << list.error << "\n"
                << "This says nothing about the app. Start Metro, then run a "
                   "debug build.\n";
      return ExitCode::kCollectionError;
    }
    if (list.targets.empty()) {
      std::cout << "Metro is running and no app is attached to its inspector.\n"
                << "A debug build connects itself. A release build runs no "
                   "inspector at all, so\nthere would be nothing to attach to "
                   "-- which is not the same as an idle app.\n";
      return ExitCode::kNotFound;
    }
    std::cout << "APP ID                                   DEVICE"
                 "                    DESCRIPTION\n";
    for (const auto& t : list.targets) {
      std::cout << std::left << std::setw(41) << elide(t.app_id, 40)
                << std::setw(26) << elide(t.device_name, 25)
                << t.description << "\n";
    }
    // Only worth saying when there is actually a choice to make.
    std::vector<std::string> devices;
    for (const auto& t : list.targets) {
      bool seen = false;
      for (const auto& d : devices) if (d == t.device_name) seen = true;
      if (!seen) devices.push_back(t.device_name);
    }
    if (devices.size() > 1) {
      std::cout << "\n" << devices.size()
                << " devices are attached. Pass --target-device with part of "
                   "a DEVICE name above\n(for example --target-device iPhone) "
                   "to say which one to observe.\n";
    }
    return ExitCode::kOk;
  }

  const observe::InspectReport report = rn::run(opts, &inv.global.cancel);

  if (inv.global.json) {
    print_json(report.to_json());
  } else {
    if (!report.debugger_attached) {
      std::cerr << "nothing was attached.\n\n";
      for (const auto& s : report.sources) {
        if (s.state == observe::SourceState::kUnavailable ||
            s.state == observe::SourceState::kRefused) {
          std::cerr << "  " << s.name << ": " << observe::to_string(s.state)
                    << "\n    " << s.detail << "\n";
        }
      }
      return ExitCode::kNotFound;
    }

    std::cout << "attached to " << report.target_title << "\n"
              << "app " << report.app_id;
    if (!report.device_name.empty()) std::cout << "  ·  " << report.device_name;
    std::cout << "  ·  observed for " << report.duration_ms << " ms\n\n";

    std::cout << "NETWORK (" << report.network.size() << ")\n";
    if (report.network.empty()) {
      std::cout << "  nothing was reported. The domain was enabled, so this is "
                   "the app making no\n  JavaScript HTTP calls in the window -- "
                   "a native module's own HTTP would not\n  appear here either "
                   "way.\n";
    } else {
      std::cout << "  METHOD  STATUS  MS        BYTES     URL\n";
      for (const auto& e : report.network) {
        std::cout << "  " << std::left << std::setw(8)
                  << (e.method.empty() ? "?" : e.method);
        // A status nobody sent is printed as a dash, never as 0.
        if (e.status.has_value()) {
          std::cout << std::setw(8) << *e.status;
        } else {
          std::cout << std::setw(8) << (e.failed ? "fail" : "-");
        }
        auto ms = e.duration_ms();
        if (ms.has_value()) {
          std::cout << std::setw(10) << std::fixed << std::setprecision(1) << *ms;
        } else {
          std::cout << std::setw(10) << "-";
        }
        if (e.encoded_bytes.has_value()) {
          std::cout << std::setw(10) << *e.encoded_bytes;
        } else {
          std::cout << std::setw(10) << "-";
        }
        std::cout << elide(e.url, 70) << "\n";
        if (e.incomplete) {
          std::cout << "          (still in flight when the window closed: "
                       "evidence of the request, none of its outcome)\n";
        }
        if (opts.capture_detail) {
          for (const auto& kv : e.request_headers) {
            std::cout << "            > " << kv.first << ": "
                      << elide(kv.second, 90) << "\n";
          }
          if (e.request_body.has_value()) {
            std::cout << "            > body "
                      << observe::InspectAssembler::single_line(*e.request_body,
                                                                100)
                      << "\n";
          }
          for (const auto& kv : e.response_headers) {
            std::cout << "            < " << kv.first << ": "
                      << elide(kv.second, 90) << "\n";
          }
          if (e.response_body.has_value()) {
            std::cout << "            < body ("
                      << (e.response_body_base64 ? "base64" : "text") << ", "
                      << e.response_body->size() << " chars) "
                      << observe::InspectAssembler::single_line(
                             *e.response_body, 90)
                      << "\n";
          } else if (!e.response_body_unavailable.empty()) {
            // A HEAD or a 204 has no body. Saying so beats an empty line
            // that reads as an empty response.
            std::cout << "            < no body: "
                      << e.response_body_unavailable << "\n";
          }
        }
        if (e.failed && !e.failure.empty()) {
          std::cout << "          failed: " << e.failure << "\n";
        }
      }
    }

    std::cout << "\nCONSOLE (" << report.console.size() << ")\n";
    if (report.console.empty()) {
      std::cout << "  the app logged nothing in this window.\n";
    } else {
      for (const auto& c : report.console) {
        std::cout << "  " << std::left << std::setw(9)
                  << (c.level.empty() ? "log" : c.level)
                  << observe::InspectAssembler::single_line(c.text, 108)
                  << "\n";
        if (c.looks_like_redux_action) {
          std::cout << "            ^ looks like the redux-logger form; "
                       "inferred action type: " << c.redux_action_type
                    << " (the app printed it; the store did not report it)\n";
        }
      }
    }

    if (opts.read_redux_state) {
      std::cout << "\nREDUX STATE\n  " << report.state.basis << "\n";
      if (report.state.found) {
        std::cout << "  " << report.state.slice_names.size() << " slice(s):";
        for (std::size_t i = 0; i < report.state.slice_names.size(); i++) {
          std::cout << (i == 0 ? " " : ", ") << report.state.slice_names[i];
        }
        std::cout << "\n  fibers walked: " << report.state.fibers_scanned << "\n";
        if (report.state.state_json.has_value()) {
          std::cout << "  values: " << report.state.state_json->size()
                    << " bytes captured (--redux-values)\n";
        } else {
          std::cout << "  values not captured; pass --redux-values to include "
                       "them, and note\n  that a store holds tokens and "
                       "personal data\n";
        }
      }
      if (!report.state.note.empty()) {
        std::cout << "  note: " << report.state.note << "\n";
      }
    }

    if (opts.watch_redux) {
      std::cout << "\nREDUX ACTIVITY (" << report.redux.records.size()
                << " record(s))\n";
      if (report.redux.dispatch_wrapped) {
        std::cout << "  dispatch was wrapped for the duration and restored "
                     "afterwards. The wrapper sits on store.dispatch, so it\n"
                     "  sees calls made through it and misses any reference "
                     "captured earlier -- a thunk's injected dispatch\n"
                     "  among them, which is why some changes below name no "
                     "action.\n";
      } else {
        std::cout << "  read-only: state changes seen through "
                     "store.subscribe, which names no action. Pass "
                     "--redux-actions\n  to see action types, which wraps "
                     "dispatch in the running app\n";
      }
      if (report.redux.dropped > 0) {
        std::cout << "  " << report.redux.dropped
                  << " record(s) were dropped by the in-app buffer: the list "
                     "below is the tail, not the whole capture\n";
      }
      if (!report.redux.restore_error.empty()) {
        std::cout << "  ! " << report.redux.restore_error << "\n";
      }
      for (const auto& rec : report.redux.records) {
        std::cout << "  " << std::setw(4) << rec.seq << "  ";
        if (rec.action_type.has_value()) {
          // Printed whole. An action type is identified by its head, and
          // `elide` keeps the tail -- right for a URL, and it turned
          // GET_ANNOUCEMENT_TIPS_REQUEST@ANNOUNCEMENT into a string starting
          // with two dots.
          std::cout << *rec.action_type;
        } else if (rec.dispatch_bypassed) {
          std::cout << "(no action named: dispatched through a reference the "
                       "wrapper does not sit on,\n        such as the dispatch "
                       "a thunk is handed)";
        } else {
          std::cout << "(state change, no action named)";
        }
        std::cout << "\n";
        if (!rec.changed_slices.empty()) {
          std::cout << "        slices:";
          for (std::size_t i = 0; i < rec.changed_slices.size(); i++) {
            std::cout << (i == 0 ? " " : ", ") << rec.changed_slices[i];
          }
          std::cout << "\n";
        }
        for (const auto& d : rec.deltas) {
          const char* mark = d.kind == observe::StateDelta::Kind::kAdded
                                 ? "+"
                                 : (d.kind == observe::StateDelta::Kind::kRemoved
                                        ? "-"
                                        : "~");
          std::cout << "        " << mark << " " << d.path;
          if (d.before.has_value() || d.after.has_value()) {
            std::cout << "   " << d.before.value_or("(absent)") << " -> "
                      << d.after.value_or("(absent)");
          }
          std::cout << "\n";
        }
        if (rec.action_payload.has_value()) {
          std::cout << "        payload: " << elide(*rec.action_payload, 100)
                    << "\n";
        }
        if (rec.equal_replacement) {
          std::cout << "        ^ the slice was replaced with an equal value: "
                       "subscribers re-rendered and nothing changed\n";
        }
        if (!rec.truncated.empty()) {
          std::cout << "        (" << rec.truncated << ")\n";
        }
      }
      if (report.redux.records.empty() && report.redux.store_found) {
        // An empty list here is a real answer, and it is not the same answer
        // as a missing store.
        std::cout << "  the store was found and nothing dispatched during "
                     "the window\n";
      }
      if (!report.redux.note.empty()) {
        std::cout << "  note: " << report.redux.note << "\n";
      }
    }

    if (!report.screenshots.empty()) {
      std::cout << "\nSCREENSHOTS (" << report.screenshots.size() << ")\n";
      for (const auto& shot : report.screenshots) {
        if (!shot.captured) {
          std::cout << "  [not taken] " << shot.error << "\n";
          continue;
        }
        std::cout << "  " << shot.path << "  " << shot.width << "x"
                  << shot.height << "  " << shot.bytes << " bytes\n"
                  << "    " << observe::evidence_note(shot.moment) << "\n";
      }
    }

    std::cout << "\nWHAT THIS DOES NOT SHOW\n";
    for (const auto& c : report.caveats) {
      std::cout << "  - " << c << "\n";
    }
  }

  // Attaching succeeded, so the command succeeded. An app that said nothing
  // is a result, not a failure -- and returning an error for it would train a
  // caller to treat silence as breakage.
  return ExitCode::kOk;
}

}  // namespace mpi::cli
