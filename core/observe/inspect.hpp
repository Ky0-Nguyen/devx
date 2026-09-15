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
// **Redux comes in two tiers, and they claim different things.** State
// *changes* are observable read-only: `store.subscribe` is how react-redux
// itself watches the store, and it fires after every dispatch. What it does
// not carry is the action -- Redux passes subscribers no arguments -- so a
// read-only record names a change and never an action.
//
// Action types and payloads require wrapping `dispatch`, which modifies the
// running app. That is opt-in, it is stated in the report, and the original
// is put back. Even then the wrapper sits on `store.dispatch` and sees only
// calls made through it: a reference captured beforehand reaches the reducers
// without passing it, and Redux Toolkit hands thunks exactly such a
// reference. A change that arrives with no action is recorded as that, never
// as one nobody was watching for, and no action is ever inferred from a diff.
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
// == Redux, in more detail than a list of slice names ==
//
// The first version of this read the store once and reported which slices
// existed. That answers "is Redux here", and almost nothing else -- the
// question people actually bring to Reactotron is "what just happened, and
// what did it change".
//
// Getting there means being precise about what is observable, because the two
// halves have genuinely different costs:
//
// **State changes are observable read-only.** `store.subscribe` is a public
// API -- it is how react-redux itself watches the store -- and it fires after
// every dispatch. Nothing is patched, and the subscription is removed at the
// end. What a subscriber does *not* receive is the action: Redux passes
// subscribers no arguments at all.
//
// **Action types and payloads require wrapping `dispatch`.** There is no
// read-only route to them; this was checked rather than assumed. So it is
// opt-in, it says what it is doing, and it puts the original `dispatch` back.
// Reactotron does the same thing -- the difference is that it needs the app
// rebuilt with a dependency, and this does not.
//
// Which of the two produced a record is carried *in* the record, because
// "the cart slice changed" and "ADD_TO_CART changed the cart slice" are
// different claims and must not be printed alike.

/// How a Redux record was obtained. Not cosmetic: it bounds what the record
/// can be read as saying.
enum class ReduxAttribution {
  /// Seen through `store.subscribe`. The state change is real; no action is
  /// attached because subscribers are passed none.
  kStateSubscription,
  /// Seen through a temporary wrapper around `store.dispatch`. The action
  /// type and payload are the ones the app dispatched.
  kDispatchWrapper,
};
const char* to_string(ReduxAttribution a);
/// The sentence a reader needs in order to not over-read a record.
const char* attribution_note(ReduxAttribution a);

/// One difference between two states, at one path.
struct StateDelta {
  enum class Kind { kAdded, kRemoved, kChanged };
  Kind kind = Kind::kChanged;
  /// Dotted path with bracketed indices: `cart.items[2].qty`.
  std::string path;
  /// The values, rendered compactly. Absent for the side a path is missing
  /// from -- an added key has no before, and a removed key has no after.
  /// Absent for *both* when values were not requested, in which case the
  /// path alone is the finding.
  std::optional<std::string> before;
  std::optional<std::string> after;

  json::Value to_json() const;
};

/// One thing that happened to the store.
struct ReduxRecord {
  std::int64_t seq = 0;
  std::int64_t at_unix_ms = 0;
  ReduxAttribution how = ReduxAttribution::kStateSubscription;
  /// Present only under kDispatchWrapper. An absent type is not "unknown
  /// action" -- it is a record that never carried one.
  std::optional<std::string> action_type;
  /// The action minus its type, bounded. Only under kDispatchWrapper, and
  /// only when values were asked for.
  std::optional<std::string> action_payload;
  /// Top-level slices whose reference changed. This is what Redux itself
  /// guarantees: a reducer that did not touch a slice returns the same
  /// object, so reference inequality is the signal react-redux uses too.
  std::vector<std::string> changed_slices;
  /// The differences within those slices, when values were requested.
  std::vector<StateDelta> deltas;
  /// Set when a slice's reference changed but its contents did not.
  ///
  /// A finding rather than an empty row. Redux's subscribers are woken by
  /// reference inequality, so a reducer that returns `{...state}` on an
  /// action it does not care about re-renders everything watching that slice
  /// while changing nothing. That is the classic source of wasted renders in
  /// a Redux app, and it is invisible in a list of action types -- it shows
  /// up here as a change with no differences under it.
  ///
  /// Only meaningful when values were captured: without them there is
  /// nothing to compare, and an empty delta list says only that nobody
  /// looked.
  bool equal_replacement = false;
  /// Set when the store was being wrapped and this change still arrived
  /// without an action.
  ///
  /// Not a failure, and not the same as not wrapping: the wrapper replaces
  /// the `dispatch` property on the store object, so it sees calls made
  /// through it -- and misses any reference captured beforehand. A Redux
  /// Toolkit thunk is the common case: the `dispatch` it is handed comes from
  /// the middleware chain built when the store was created, so everything a
  /// thunk dispatches reaches the reducers without passing the wrapper.
  ///
  /// Recorded because the alternative is a record that looks like the
  /// read-only case and quietly implies nothing was wrapped.
  bool dispatch_bypassed = false;
  /// Set when the record is known to be incomplete -- a truncated payload, a
  /// delta list that hit its cap, a slice too large to stringify.
  std::string truncated;

  json::Value to_json() const;
};

/// Everything read about the store in one capture.
struct ReduxObservation {
  bool store_found = false;
  std::string basis;
  std::vector<std::string> slice_names;
  std::int64_t fibers_scanned = 0;
  /// The state as it was when the probe attached, when values were asked
  /// for. Bounded.
  std::optional<std::string> initial_state_json;
  std::vector<ReduxRecord> records;
  /// How many notifications arrived beyond the buffer's capacity. Non-zero
  /// means the list below is a *tail*, not the whole capture, and saying so
  /// is the difference between a short list and a wrong one.
  std::int64_t dropped = 0;
  bool dispatch_wrapped = false;
  /// Set when the wrapper could not be removed. This matters to the person
  /// whose app it is: a wrapper left installed keeps recording into a buffer
  /// nobody drains until the app reloads.
  std::string restore_error;
  std::string note;

  json::Value to_json() const;
};

/// Differences between two JSON states.
///
/// Pure, so it can be tested on values rather than on a running app. Bounded
/// on both depth and count: a store is arbitrarily deep, and a diff that
/// walked all of it would be this tool causing the stall it exists to find.
/// Hitting either bound is reported through `truncated_out` rather than
/// silently shortening the list.
///
/// `max_depth` counts **path segments**: at 3, `cart.items[0]` is as specific
/// as a reported path gets, and a change below it is reported there with the
/// truncation note saying so.
///
/// Arrays are compared by index. That is the honest cheap answer: an element
/// inserted at the front reports every later index as changed, which is true
/// -- index 3 really does hold something different -- even though a person
/// would describe it as one insertion.
std::vector<StateDelta> diff_state(const json::Value& before,
                                   const json::Value& after,
                                   bool include_values,
                                   std::size_t max_deltas,
                                   std::size_t max_depth,
                                   std::string* truncated_out);

/// A value rendered for a delta: short, single-line, and never silently cut.
///
/// A container is summarised by shape rather than dumped -- `{7 keys}`,
/// `[12 items]` -- because the point of a delta is the path, and an inlined
/// subtree buries it. A long string is cut with an explicit marker.
std::string render_delta_value(const json::Value& v, std::size_t max_chars);

/// Folds one drain result from the in-app watcher into an observation.
///
/// Pure, and separate from the socket, so the whole translation from "what
/// the app sent" to "what the report claims" is testable on recorded JSON.
/// The app sends the changed slices as two JSON strings; the deltas between
/// them are computed here rather than in the app, because a diff is exactly
/// the kind of logic that needs tests and the app is not a place that can
/// have any.
///
/// Anything unrecognised is ignored rather than guessed at: this parses a
/// payload produced inside someone else's runtime, where a reload can change
/// the shape underneath us mid-capture.
void ingest_redux_drain(const json::Value& drain_result, bool include_values,
                        ReduxObservation* out);

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
  /// What happened to the store while the capture was open. Separate from
  /// `state` above, which is one reading at one moment: a list of changes and
  /// a snapshot answer different questions and neither substitutes for the
  /// other.
  ReduxObservation redux;

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
