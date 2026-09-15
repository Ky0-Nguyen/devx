#include "core/observe/inspect.hpp"

#include <algorithm>
#include <cmath>

namespace mpi::observe {
namespace {

const std::string kEmpty;

const std::string& str(const json::Value* v) {
  return (v != nullptr && v->is_string()) ? v->as_string() : kEmpty;
}

const json::Value* field(const json::Value& v, std::string_view key) {
  return v.is_object() ? v.find(key) : nullptr;
}

/// Copies a CDP header object. Values are kept verbatim -- redacting a token
/// here would be a claim about what was sent that is not true.
std::map<std::string, std::string> read_headers(const json::Value* headers) {
  std::map<std::string, std::string> out;
  if (headers == nullptr || !headers->is_object()) return out;
  for (const auto& member : headers->members()) {
    out[member.first] = member.second.is_string() ? member.second.as_string()
                                                  : member.second.dump();
  }
  return out;
}

/// CDP timestamps are seconds as a double (monotonic, not wall clock). The
/// model keeps nanoseconds, and the conversion is done once here rather than
/// at each use so a unit mistake cannot be made twice.
model::TimeNs seconds_to_ns(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) return 0;
  return static_cast<model::TimeNs>(seconds * 1e9);
}

}  // namespace

const char* to_string(SourceState s) {
  switch (s) {
    case SourceState::kUnavailable:   return "unavailable";
    case SourceState::kAttached:      return "attached";
    case SourceState::kRanSawNothing: return "ran_saw_nothing";
    case SourceState::kRefused:       return "refused";
  }
  return "unavailable";
}

std::optional<double> NetworkExchange::duration_ms() const {
  if (!finished_ns.has_value() || started_ns == 0) return std::nullopt;
  if (*finished_ns < started_ns) return std::nullopt;   // clock went backwards
  return static_cast<double>(*finished_ns - started_ns) / 1e6;
}

std::string InspectAssembler::single_line(const std::string& s,
                                          std::size_t limit) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char c : s) {
    if (c == '\n' || c == '\r' || c == '\t') {
      // One separator per run of whitespace, and a visible one: a log line
      // that was three lines is not the same as one that was written on one,
      // and silently joining them reads as a single sentence.
      pending_space = true;
      continue;
    }
    if (pending_space) {
      if (!out.empty()) out += " / ";
      pending_space = false;
    }
    out += c;
  }
  if (out.size() > limit && limit > 2) {
    out = out.substr(0, limit - 2) + "..";
  }
  return out;
}

std::string InspectAssembler::strip_ansi(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c != 0x1B) {
      out += s[i];
      continue;
    }
    // CSI: ESC '[' … final byte in 0x40..0x7E. Anything else after ESC is a
    // sequence this does not model, and the ESC alone is dropped rather than
    // guessed past -- dropping one byte is recoverable, skipping to a wrong
    // terminator eats real text.
    if (i + 1 < s.size() && s[i + 1] == '[') {
      std::size_t j = i + 2;
      while (j < s.size()) {
        unsigned char f = static_cast<unsigned char>(s[j]);
        if (f >= 0x40 && f <= 0x7E) break;
        j++;
      }
      i = (j < s.size()) ? j : s.size() - 1;
    }
  }
  return out;
}

std::string InspectAssembler::join_console_args(const json::Value& args) {
  if (!args.is_array()) return {};
  std::string out;
  for (const json::Value& arg : args.items()) {
    if (!out.empty()) out += " ";
    const json::Value* value = field(arg, "value");
    if (value != nullptr) {
      if (value->is_string()) {
        out += value->as_string();
      } else if (value->is_bool()) {
        out += value->as_bool() ? "true" : "false";
      } else if (value->is_number()) {
        out += value->dump();
      } else if (value->is_null()) {
        out += "null";
      } else {
        // An object passed by value: render it rather than describing it.
        out += value->dump();
      }
      continue;
    }
    // No value: a remote object. `description` is the runtime's own rendering
    // ("Object", "Array(3)", an Error's stack). `preview` carries the fields.
    const json::Value* desc = field(arg, "description");
    if (desc != nullptr && desc->is_string() && !desc->as_string().empty()) {
      out += desc->as_string();
      continue;
    }
    const json::Value* preview = field(arg, "preview");
    if (preview != nullptr) {
      const json::Value* d = field(*preview, "description");
      if (d != nullptr && d->is_string()) {
        out += d->as_string();
        continue;
      }
    }
    // An argument with nothing renderable still happened, and an empty string
    // would lose it. Name the type instead.
    const std::string& type = str(field(arg, "type"));
    out += "<" + (type.empty() ? std::string("unknown") : type) + ">";
  }
  return out;
}

std::optional<std::string> InspectAssembler::redux_action_type(
    const std::string& text) {
  // redux-logger prints "action <TYPE> @ HH:MM:SS.mmm", and its collapsed
  // form prints "action  <TYPE>". Matched on that literal prefix only: a
  // looser rule would claim any line containing the word "action".
  static const char* kPrefixes[] = {"action ", "action  "};
  for (const char* prefix : kPrefixes) {
    std::size_t n = std::string(prefix).size();
    if (text.size() <= n || text.compare(0, n, prefix) != 0) continue;
    std::size_t start = text.find_first_not_of(' ', n);
    if (start == std::string::npos) continue;
    std::size_t end = text.find_first_of(" \t", start);
    std::string type = text.substr(start, end == std::string::npos
                                              ? std::string::npos
                                              : end - start);
    // An action type is a token. Anything with a space in it is prose that
    // happened to begin with the word.
    if (type.empty() || type.size() > 120) continue;
    if (type.find_first_of("{}()\"") != std::string::npos) continue;
    return type;
  }
  return std::nullopt;
}

void InspectAssembler::set_response_body(const std::string& request_id,
                                         std::string body, bool base64) {
  auto it = exchanges_.find(request_id);
  if (it == exchanges_.end()) return;
  it->second.response_body = std::move(body);
  it->second.response_body_base64 = base64;
  it->second.response_body_unavailable.clear();
}

void InspectAssembler::set_response_body_unavailable(
    const std::string& request_id, std::string reason) {
  auto it = exchanges_.find(request_id);
  if (it == exchanges_.end()) return;
  const NetworkExchange& ex = it->second;
  // "There is no body" and "the body could not be retrieved" are different
  // facts, and the runtime reports both with the same error. Where the
  // exchange itself settles it, say so plainly: the raw "Internal error"
  // reads as a failure for a response that was never going to have content.
  const auto content_length = ex.response_headers.find("Content-Length");
  const bool empty_by_construction =
      ex.method == "HEAD" || ex.status.value_or(0) == 204 ||
      ex.status.value_or(0) == 304 ||
      (content_length != ex.response_headers.end() &&
       content_length->second == "0");
  if (empty_by_construction) {
    it->second.response_body_unavailable =
        "this response has no body by construction (" +
        (ex.method == "HEAD" ? std::string("a HEAD request")
                             : "HTTP " + std::to_string(ex.status.value_or(0))) +
        "), so there was nothing to retrieve";
    return;
  }
  it->second.response_body_unavailable = std::move(reason);
}

void InspectAssembler::feed(const json::Value& message) {
  const std::string& method = str(field(message, "method"));
  if (method.empty()) return;   // a command reply, not an event
  const json::Value* p = field(message, "params");
  if (p == nullptr) {
    unrecognised_++;
    return;
  }
  const json::Value& params = *p;

  if (method == "Network.requestWillBeSent") {
    network_events_++;
    const std::string& id = str(field(params, "requestId"));
    if (id.empty()) { unrecognised_++; return; }
    const json::Value* req = field(params, "request");
    NetworkExchange& ex = exchanges_[id];
    if (ex.request_id.empty()) {
      ex.request_id = id;
      order_.push_back(id);
    }
    if (req != nullptr) {
      ex.method = str(field(*req, "method"));
      ex.url = str(field(*req, "url"));
      if (capture_detail_) {
        ex.request_headers = read_headers(field(*req, "headers"));
        const json::Value* post = field(*req, "postData");
        // Only when there is one. An empty string is not a body, and
        // recording it produced a blank "body" line that read as a request
        // that sent nothing deliberately.
        if (post != nullptr && post->is_string() && !post->as_string().empty()) {
          ex.request_body = post->as_string();
        }
      }
    }
    const json::Value* ts = field(params, "timestamp");
    if (ts != nullptr && ts->is_number()) {
      double seconds = ts->as_double();
      if (!first_timestamp_.has_value()) first_timestamp_ = seconds;
      ex.started_ns = seconds_to_ns(seconds);
    }
    return;
  }
  if (method == "Network.responseReceived") {
    network_events_++;
    const std::string& id = str(field(params, "requestId"));
    auto it = exchanges_.find(id);
    if (it == exchanges_.end()) {
      // A response for a request that began before we attached. It is real,
      // so it is kept -- with no start time, which duration_ms() then
      // refuses to turn into a number.
      if (id.empty()) { unrecognised_++; return; }
      NetworkExchange fresh;
      fresh.request_id = id;
      order_.push_back(id);
      it = exchanges_.emplace(id, fresh).first;
    }
    const json::Value* resp = field(params, "response");
    if (resp != nullptr) {
      const json::Value* status = field(*resp, "status");
      if (status != nullptr && status->is_number()) {
        it->second.status = static_cast<int>(status->as_int());
      }
      it->second.mime_type = str(field(*resp, "mimeType"));
      if (it->second.url.empty()) it->second.url = str(field(*resp, "url"));
      if (capture_detail_) {
        it->second.response_headers = read_headers(field(*resp, "headers"));
      }
    }
    return;
  }
  if (method == "Network.loadingFinished") {
    network_events_++;
    auto it = exchanges_.find(str(field(params, "requestId")));
    if (it == exchanges_.end()) { unrecognised_++; return; }
    const json::Value* ts = field(params, "timestamp");
    if (ts != nullptr && ts->is_number()) {
      it->second.finished_ns = seconds_to_ns(ts->as_double());
    }
    const json::Value* len = field(params, "encodedDataLength");
    if (len != nullptr && len->is_number()) {
      it->second.encoded_bytes = static_cast<std::int64_t>(len->as_double());
    }
    // The caller asks for the body; this only records that there is one to
    // ask about. Recorded whether or not detail is on, so switching it on
    // mid-session does not need the list rebuilt.
    finished_.push_back(it->first);
    return;
  }
  if (method == "Network.loadingFailed") {
    network_events_++;
    auto it = exchanges_.find(str(field(params, "requestId")));
    if (it == exchanges_.end()) { unrecognised_++; return; }
    it->second.failed = true;
    it->second.failure = str(field(params, "errorText"));
    const json::Value* ts = field(params, "timestamp");
    if (ts != nullptr && ts->is_number()) {
      it->second.finished_ns = seconds_to_ns(ts->as_double());
    }
    return;
  }
  // Carries cookie and header detail for a request already reported. Counted
  // as a network event so the source's event total matches what arrived, but
  // it adds nothing this model keeps.
  if (method == "Network.requestWillBeSentExtraInfo" ||
      method == "Network.responseReceivedExtraInfo" ||
      method == "Network.dataReceived" ||
      method == "Network.resourceChangedPriority") {
    network_events_++;
    return;
  }

  if (method == "Runtime.consoleAPICalled" || method == "Log.entryAdded") {
    console_events_++;
    ConsoleEntry entry;
    if (method == "Log.entryAdded") {
      const json::Value* e = field(params, "entry");
      if (e == nullptr) { unrecognised_++; return; }
      entry.level = str(field(*e, "level"));
      entry.text = strip_ansi(str(field(*e, "text")));
      entry.source_url = str(field(*e, "url"));
      const json::Value* ts = field(*e, "timestamp");
      if (ts != nullptr && ts->is_number()) {
        // Log.entryAdded timestamps are milliseconds since the epoch, unlike
        // Network's seconds. Converting both as seconds put console lines in
        // 1970 next to requests in the present.
        entry.timestamp_ns = static_cast<model::TimeNs>(ts->as_double() * 1e6);
      }
    } else {
      entry.level = str(field(params, "type"));
      const json::Value* args = field(params, "args");
      entry.text = args != nullptr ? strip_ansi(join_console_args(*args))
                                   : std::string();
      const json::Value* ts = field(params, "timestamp");
      if (ts != nullptr && ts->is_number()) {
        entry.timestamp_ns = static_cast<model::TimeNs>(ts->as_double() * 1e6);
      }
      const json::Value* stack = field(params, "stackTrace");
      if (stack != nullptr) {
        const json::Value* frames = field(*stack, "callFrames");
        if (frames != nullptr && frames->is_array() && !frames->items().empty()) {
          const json::Value& top = frames->items().front();
          entry.source_url = str(field(top, "url"));
          const json::Value* line = field(top, "lineNumber");
          if (line != nullptr && line->is_number()) {
            entry.line = static_cast<int>(line->as_int());
          }
        }
      }
    }
    if (auto type = redux_action_type(entry.text)) {
      entry.looks_like_redux_action = true;
      entry.redux_action_type = *type;
    }
    console_.push_back(std::move(entry));
    return;
  }

  if (method == "Runtime.exceptionThrown") {
    console_events_++;
    ConsoleEntry entry;
    entry.level = "error";
    const json::Value* d = field(params, "exceptionDetails");
    if (d != nullptr) {
      std::string text = str(field(*d, "text"));
      const json::Value* ex = field(*d, "exception");
      if (ex != nullptr) {
        const std::string& desc = str(field(*ex, "description"));
        if (!desc.empty()) text = desc;
      }
      entry.text = strip_ansi(text);
      entry.source_url = str(field(*d, "url"));
      const json::Value* line = field(*d, "lineNumber");
      if (line != nullptr && line->is_number()) {
        entry.line = static_cast<int>(line->as_int());
      }
    }
    const json::Value* ts = field(params, "timestamp");
    if (ts != nullptr && ts->is_number()) {
      entry.timestamp_ns = static_cast<model::TimeNs>(ts->as_double() * 1e6);
    }
    console_.push_back(std::move(entry));
    return;
  }

  // Housekeeping that says nothing about the app.
  if (method == "Runtime.executionContextCreated" ||
      method == "Runtime.executionContextDestroyed" ||
      method == "Runtime.executionContextsCleared" ||
      method == "Debugger.scriptParsed" ||
      method == "Debugger.scriptFailedToParse" ||
      method == "Page.frameNavigated" ||
      method == "ReactNativeApplication.metadataUpdated") {
    return;
  }
  unrecognised_++;
}

InspectReport InspectAssembler::finish(std::int64_t wall_ms) {
  InspectReport report;
  report.duration_ms = wall_ms;

  for (const std::string& id : order_) {
    auto it = exchanges_.find(id);
    if (it == exchanges_.end()) continue;
    NetworkExchange ex = it->second;
    // No end and no failure: it was still in flight. That is evidence of a
    // request and no evidence about its outcome, which the flag says rather
    // than a zero duration implying an instant reply.
    if (!ex.finished_ns.has_value() && !ex.failed) ex.incomplete = true;
    report.network.push_back(std::move(ex));
  }

  report.console = console_;
  std::stable_sort(report.console.begin(), report.console.end(),
                   [](const ConsoleEntry& a, const ConsoleEntry& b) {
                     return a.timestamp_ns < b.timestamp_ns;
                   });

  // Each source reports its own silence. "Attached and saw nothing" is a
  // statement about the app; "unavailable" is a statement about us.
  SourceStatus net;
  net.name = "network (CDP Network domain)";
  net.events = network_events_;
  net.state = network_events_ > 0 ? SourceState::kAttached
                                  : SourceState::kRanSawNothing;
  net.detail = network_events_ > 0
      ? "React Native's own fetch/XHR instrumentation reported these"
      : "the domain was enabled and no request was reported. This covers "
        "JavaScript fetch/XHR only, so it would be empty either way for a "
        "screen whose traffic is native: a WebView (an SSO or payment page "
        "is usually one), a native networking module, or the platform's "
        "image loader. Measured on a React Native SSO login: typing and "
        "submitting the form produced no entry here at all.";
  report.sources.push_back(net);

  SourceStatus con;
  con.name = "console (CDP Runtime/Log domains)";
  con.events = console_events_;
  con.state = console_events_ > 0 ? SourceState::kAttached
                                  : SourceState::kRanSawNothing;
  con.detail = console_events_ > 0
      ? "whatever the app chose to log"
      : "the domains were enabled and the app logged nothing in this window";
  report.sources.push_back(con);

  report.caveats = {
      "This needs a debug build: a release build runs no inspector, so an "
      "empty capture there means there was nothing to attach to, not that "
      "the app was idle.",
      "Network coverage is the JavaScript side only: React Native's own "
      "fetch/XHR instrumentation. A **WebView** does not appear here -- which "
      "covers most SSO and payment flows -- and neither does a native "
      "networking module or the platform image loader. An empty list does "
      "not mean the app made no requests.",
      "Redux actions are not observable without modifying the running app. "
      "The store's state can be read; the stream of dispatched actions "
      "cannot, and nothing here infers actions from state changes.",
      "A debugger was attached for the duration. That changes what the "
      "runtime does -- Hermes may deoptimise -- so nothing in this report is "
      "a performance measurement.",
  };
  return report;
}

json::Value InspectReport::to_json() const {
  json::Value out = json::Value::object();
  out.set("app_id", json::Value::string(app_id));
  out.set("device_name", json::Value::string(device_name));
  out.set("target_title", json::Value::string(target_title));
  out.set("duration_ms", json::Value::integer(duration_ms));
  out.set("debugger_attached", json::Value::boolean(debugger_attached));

  json::Value srcs = json::Value::array();
  for (const SourceStatus& s : sources) {
    json::Value v = json::Value::object();
    v.set("name", json::Value::string(s.name));
    v.set("state", json::Value::string(to_string(s.state)));
    v.set("detail", json::Value::string(s.detail));
    v.set("events", json::Value::integer(s.events));
    srcs.push_back(std::move(v));
  }
  out.set("sources", std::move(srcs));

  json::Value net = json::Value::array();
  for (const NetworkExchange& e : network) {
    json::Value v = json::Value::object();
    v.set("request_id", json::Value::string(e.request_id));
    v.set("method", json::Value::string(e.method));
    v.set("url", json::Value::string(e.url));
    // Absent rather than zero: nobody sent a status, and 0 is a status.
    v.set("status", e.status.has_value()
                        ? json::Value::integer(*e.status)
                        : json::Value::null());
    v.set("mime_type", json::Value::string(e.mime_type));
    v.set("encoded_bytes", e.encoded_bytes.has_value()
                               ? json::Value::integer(*e.encoded_bytes)
                               : json::Value::null());
    auto ms = e.duration_ms();
    v.set("duration_ms", ms.has_value() ? json::Value::number(*ms)
                                        : json::Value::null());
    v.set("incomplete", json::Value::boolean(e.incomplete));
    if (!e.request_headers.empty()) {
      json::Value h = json::Value::object();
      for (const auto& kv : e.request_headers) {
        h.set(kv.first, json::Value::string(kv.second));
      }
      v.set("request_headers", std::move(h));
    }
    if (!e.response_headers.empty()) {
      json::Value h = json::Value::object();
      for (const auto& kv : e.response_headers) {
        h.set(kv.first, json::Value::string(kv.second));
      }
      v.set("response_headers", std::move(h));
    }
    if (e.request_body.has_value()) {
      v.set("request_body", json::Value::string(*e.request_body));
    }
    if (e.response_body.has_value()) {
      v.set("response_body", json::Value::string(*e.response_body));
      // The runtime decides text versus base64, and the flag has to travel
      // with the value: a reader decoding text as base64 gets nonsense.
      v.set("response_body_base64",
            json::Value::boolean(e.response_body_base64));
    } else if (!e.response_body_unavailable.empty()) {
      v.set("response_body_unavailable",
            json::Value::string(e.response_body_unavailable));
    }
    v.set("failed", json::Value::boolean(e.failed));
    if (!e.failure.empty()) v.set("failure", json::Value::string(e.failure));
    net.push_back(std::move(v));
  }
  out.set("network", std::move(net));

  json::Value con = json::Value::array();
  for (const ConsoleEntry& c : console) {
    json::Value v = json::Value::object();
    v.set("timestamp_ns", json::Value::integer(c.timestamp_ns));
    v.set("level", json::Value::string(c.level));
    v.set("text", json::Value::string(c.text));
    if (!c.source_url.empty()) {
      v.set("source_url", json::Value::string(c.source_url));
      v.set("line", json::Value::integer(c.line));
    }
    if (c.looks_like_redux_action) {
      // Named so the reader cannot mistake it for a dispatched action the
      // store reported: this is redux-logger's printed output, recognised.
      v.set("inferred_redux_action_type",
            json::Value::string(c.redux_action_type));
      v.set("inference_basis",
            json::Value::string("the app printed it in redux-logger's format; "
                                "the store did not report it"));
    }
    con.push_back(std::move(v));
  }
  out.set("console", std::move(con));

  json::Value st = json::Value::object();
  st.set("found", json::Value::boolean(state.found));
  st.set("basis", json::Value::string(state.basis));
  st.set("fibers_scanned", json::Value::integer(state.fibers_scanned));
  json::Value slices = json::Value::array();
  for (const std::string& s : state.slice_names) {
    slices.push_back(json::Value::string(s));
  }
  st.set("slice_names", std::move(slices));
  if (state.state_json.has_value()) {
    st.set("state_json", json::Value::string(*state.state_json));
  }
  if (!state.note.empty()) st.set("note", json::Value::string(state.note));
  out.set("redux_state", std::move(st));

  json::Value shots = json::Value::array();
  for (const Screenshot& s : screenshots) shots.push_back(s.to_json());
  out.set("screenshots", std::move(shots));

  json::Value cav = json::Value::array();
  for (const std::string& c : caveats) cav.push_back(json::Value::string(c));
  out.set("caveats", std::move(cav));
  return out;
}

}  // namespace mpi::observe
