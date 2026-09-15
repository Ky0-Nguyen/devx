// Reading a running app's network calls, console output and Redux state
// without adding anything to the app.
//
// The request this answers is "Reactotron, but nothing installed". Reactotron
// works by having the app import a client and open a socket; the whole point
// here is that the app is untouched -- no dependency, no import, no rebuild.
//
// That is possible because a React Native **debug** build already runs an
// inspector and already connects itself to Metro. Metro proxies a Chrome
// DevTools Protocol session, and CDP carries exactly the three things asked
// for. Measured against a real app rather than assumed:
//
//   Network.requestWillBeSent / responseReceived / loadingFinished
//       every fetch and XMLHttpRequest, with method, URL, status, size and
//       timing.
//   Runtime.consoleAPICalled
//       every console.log / warn / error, with its arguments.
//   Runtime.evaluate
//       the Redux store, located by walking React's own devtools hook to a
//       Provider and reading `store.getState()`.
//
// == What this is not ==
//
// Four limits, each of which would otherwise turn "we saw nothing" into the
// false claim "nothing happened". They are carried in the report, not just
// written here.
//
// **It needs the inspector.** A release build has none: there is no target to
// attach to, and that is a missing provider, not a quiet app. Verified: the
// target list is empty until a debug build connects.
//
// **Network coverage is the JS side only.** These events come from React
// Native's own fetch/XHR instrumentation. An HTTP call made by a native
// module through OkHttp, or an image fetched by the platform's loader, is
// invisible here. "No requests" therefore does not mean the app made none.
//
// **Redux actions are not observable read-only.** The store's *state* can be
// read; the stream of actions cannot, because nothing broadcasts it. Seeing
// actions would mean wrapping `dispatch` in the running app -- modifying it,
// which is the thing this feature exists to avoid -- so it is not done, and
// the gap is reported rather than filled with inference from state diffs.
//
// **Attaching is not free.** A debugger session changes what the runtime
// does: Hermes may deoptimise, and timings taken while attached are not
// timings of the app running alone. Nothing here is offered as a performance
// measurement, and a capture records that a debugger was attached.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model/trace.hpp"
#include "core/observe/screenshot.hpp"
#include "core/util/json.hpp"

namespace mpi::observe {

/// Whether a source ran, and what its silence means.
///
/// The distinction the whole project turns on, applied here: a source that
/// was never reachable and a source that ran and saw nothing are different
/// answers, and only one of them says anything about the app.
enum class SourceState {
  kUnavailable,   // could not be reached at all
  kAttached,      // connected and listening
  kRanSawNothing, // connected, and genuinely nothing arrived
  kRefused,       // reachable but declined the request
};
const char* to_string(SourceState s);

struct SourceStatus {
  std::string name;
  SourceState state = SourceState::kUnavailable;
  std::string detail;         // why, in the source's own words
  std::int64_t events = 0;
};

/// One HTTP exchange, assembled from the three events that describe it.
///
/// `status` and `finished` are optional because a request in flight when the
/// capture ended has neither, and reporting 0 for a status nobody sent would
/// invent a failed request.
struct NetworkExchange {
  std::string request_id;
  std::string method;
  std::string url;
  std::optional<int> status;
  std::string mime_type;
  std::optional<std::int64_t> encoded_bytes;
  model::TimeNs started_ns = 0;
  std::optional<model::TimeNs> finished_ns;
  bool failed = false;
  std::string failure;
  /// Set when the capture ended before the response did. Such an exchange is
  /// evidence of a request, and no evidence at all about its outcome.
  bool incomplete = false;

  // --- detail, captured only when asked for ------------------------------
  //
  // Headers and bodies are what makes a request inspectable rather than just
  // listed, and they are also where the secrets are: an `Authorization`
  // header carries a bearer token, and a login response carries whatever the
  // login returned. They are off by default for the same reason the Redux
  // store's values are, and for a stronger one -- this is the data in flight,
  // not the data at rest.
  std::map<std::string, std::string> request_headers;
  std::map<std::string, std::string> response_headers;
  /// The request body, when the request had one (`request.postData`).
  std::optional<std::string> request_body;
  /// The response body, fetched with `Network.getResponseBody`.
  std::optional<std::string> response_body;
  /// Whether `response_body` is base64. The runtime decides: text comes back
  /// as text and anything else base64, and re-encoding it here would destroy
  /// the distinction.
  bool response_body_base64 = false;
  /// Set when a body was asked for and the runtime had none to give. A HEAD
  /// or a 204 genuinely has no body, and that is not a failure to record.
  std::string response_body_unavailable;

  std::optional<double> duration_ms() const;
};

/// One console call.
struct ConsoleEntry {
  model::TimeNs timestamp_ns = 0;
  std::string level;          // the runtime's own word: log, warning, error, info
  std::string text;           // arguments joined, terminal colouring removed
  std::string source_url;
  int line = 0;
  /// A console call the app made that looks like a Redux action, by the shape
  /// redux-logger prints. A guess, and labelled as one wherever it is shown.
  bool looks_like_redux_action = false;
  std::string redux_action_type;
};

/// A read of the Redux store.
struct StateSnapshot {
  bool found = false;
  /// How the store was located, so a reader knows what the value rests on.
  std::string basis;
  std::vector<std::string> slice_names;
  std::int64_t fibers_scanned = 0;
  /// The state itself, when asked for. Omitted by default: an app's store
  /// holds tokens and personal data, and writing it into a session package
  /// that gets exported is not something to do without being asked.
  std::optional<std::string> state_json;
  std::string note;
};

struct InspectReport {
  std::string app_id;
  std::string device_name;
  std::string target_title;
  std::int64_t duration_ms = 0;
  bool debugger_attached = true;

  std::vector<SourceStatus> sources;
  std::vector<NetworkExchange> network;
  std::vector<ConsoleEntry> console;
  StateSnapshot state;

  /// Pictures of the screen, each stating when it was taken and therefore
  /// what it is evidence of. See screenshot.hpp: an image is never offered
  /// as the moment a finding refers to.
  std::vector<Screenshot> screenshots;

  /// The limits above, as text that travels with the data.
  std::vector<std::string> caveats;

  json::Value to_json() const;
};

/// Turns a stream of CDP messages into a report.
///
/// Separate from the transport so it can be driven from a recorded capture in
/// a test: the fixture under fixtures/cdp is a real session, and this is the
/// part that has to survive the shapes a real runtime produces.
class InspectAssembler {
 public:
  /// Feeds one CDP message. Unknown methods are counted, never guessed at.
  void feed(const json::Value& message);

  /// Whether headers and bodies are kept. Off by default: they carry bearer
  /// tokens and whatever a login returned.
  void capture_detail(bool on) { capture_detail_ = on; }
  bool capturing_detail() const { return capture_detail_; }

  /// Records a body fetched separately with `Network.getResponseBody`.
  ///
  /// Separate from `feed` because the reply to that command carries an `id`
  /// and no method, so only the caller that sent it knows which request it
  /// belongs to.
  void set_response_body(const std::string& request_id, std::string body,
                         bool base64);
  /// Records that the runtime had no body to give for a request.
  void set_response_body_unavailable(const std::string& request_id,
                                     std::string reason);

  /// The requests whose responses have finished, in arrival order. The caller
  /// asks for their bodies; the assembler does not talk to anything.
  const std::vector<std::string>& finished_requests() const {
    return finished_;
  }

  /// Finishes the report. `wall_ms` is the capture's own duration.
  InspectReport finish(std::int64_t wall_ms);

  std::int64_t unrecognised() const { return unrecognised_; }

  /// Collapses a multi-line message onto one line.
  ///
  /// Console arguments are often pretty-printed JSON, and a table row that
  /// grows to fifteen lines destroys the table. The full text is kept in the
  /// JSON output; this is only for fixed-width rendering.
  static std::string single_line(const std::string& s, std::size_t limit);

  /// Strips ANSI SGR sequences.
  ///
  /// Real console output is full of them -- React Native colours its own
  /// warnings -- and a report carrying raw escape bytes is unreadable in a
  /// document and corrupts a terminal.
  static std::string strip_ansi(const std::string& s);

  /// Joins a `consoleAPICalled` argument list into text.
  ///
  /// Arguments arrive as remote objects: primitives carry `value`, objects
  /// carry `description` or a `preview`. A missing value is rendered as the
  /// type name in angle brackets rather than as an empty string, so a log
  /// line does not silently lose an argument.
  static std::string join_console_args(const json::Value& args);

  /// Recognises redux-logger's printed form, returning the action type.
  ///
  /// A convention, not a signal: the app chose to print it and could stop.
  /// The caller labels anything built on this as inferred.
  static std::optional<std::string> redux_action_type(const std::string& text);

 private:
  std::map<std::string, NetworkExchange> exchanges_;
  std::vector<std::string> order_;
  std::vector<ConsoleEntry> console_;
  std::int64_t unrecognised_ = 0;
  std::int64_t network_events_ = 0;
  std::int64_t console_events_ = 0;
  std::optional<double> first_timestamp_;
  bool capture_detail_ = false;
  std::vector<std::string> finished_;
};

}  // namespace mpi::observe
