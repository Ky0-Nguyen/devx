// The Model Context Protocol, enough of it to be a server.
//
// The request: let Cursor, Claude, Codex and anything else that speaks MCP
// read a capture and act on it, so the analysis can be discussed with a model
// that has the actual numbers rather than a screenshot of them.
//
// == Why this is written here rather than imported ==
//
// ADR-0002 keeps third-party libraries out of this tree, and the official MCP
// SDKs are TypeScript and Python -- neither of which this project has a
// runtime for. What MCP actually requires over stdio is JSON-RPC 2.0 with
// newline-delimited messages, and this repo already has a JSON parser and
// writer. So the protocol is written, and the omissions are listed rather
// than discovered:
//
//   * stdio transport only. No SSE, no HTTP. A local tool talking to a local
//     editor needs no socket, and a socket is an attack surface.
//   * tools and resources. No prompts, no sampling, no roots, no completion:
//     nothing here would fill them, and advertising a capability that returns
//     an empty list is worse than not advertising it.
//   * no batching. The spec allows a JSON-RPC batch and no host sends one to
//     a stdio server; a batch arrives as an error saying so rather than being
//     half-handled.
//
// == What it will and will not do ==
//
// Reading is free. Acting is not: recording a capture starts processes on a
// device, booting a simulator changes the machine's state, and a suppression
// edits a file the next analysis reads. Those tools exist and are refused
// unless the server was started with actions enabled, because a model that
// decides to "just try recording" on its own is a different thing from a
// person asking for it. The refusal names the flag rather than pretending the
// tool does not exist.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::mcp {

/// The protocol revision this server implements.
///
/// Sent back verbatim from `initialize`. A host asking for a different
/// revision is answered with this one rather than refused: the spec's own
/// negotiation is "the server replies with the version it will use", and a
/// host that cannot live with it disconnects.
inline constexpr const char* kProtocolVersion = "2024-11-05";

/// Whether a tool only reads, or changes something.
enum class Effect {
  kReadOnly,
  /// Starts a process, changes the machine, or writes a file. Refused unless
  /// the server was started with actions enabled.
  kMutating,
};

/// One tool the server offers.
struct Tool {
  std::string name;
  /// What it does and, where it matters, what it does not. This text is the
  /// only thing a model reads before choosing, so a limit left out here is a
  /// limit the model will not know about.
  std::string description;
  /// JSON Schema for the arguments, as MCP requires.
  json::Value input_schema;
  Effect effect = Effect::kReadOnly;
  /// Runs it. Receives the `arguments` object, returns the text to hand back.
  std::function<std::string(const json::Value&)> run;
};

/// The outcome of one tool call, before it becomes a protocol response.
struct ToolResult {
  std::string text;
  /// True when the tool ran and reported a failure. Distinct from a protocol
  /// error: the call was valid and the answer is that something went wrong,
  /// which the model should read rather than retry.
  bool is_error = false;
};

/// A dispatcher for one connection.
///
/// Pure with respect to I/O: it takes a parsed request and returns the
/// response to write, or nothing for a notification. Every test drives it by
/// handing it JSON, which is why the transport is a dozen lines elsewhere.
class Server {
 public:
  explicit Server(bool allow_actions) : allow_actions_(allow_actions) {}

  void add_tool(Tool tool);

  /// Handles one message.
  ///
  /// Returns the response, or nullopt when the message was a notification --
  /// JSON-RPC forbids replying to one, and a host that receives a reply to
  /// its `notifications/initialized` may drop the connection.
  std::optional<json::Value> handle(const json::Value& message);

  /// Whether `initialize` has been seen. A host must send it first; a tool
  /// call before it is answered with an error rather than served, because a
  /// server that works without initialising trains hosts to skip it.
  bool initialized() const { return initialized_; }

  const std::vector<Tool>& tools() const { return tools_; }

 private:
  json::Value initialize_result() const;
  json::Value tools_list_result() const;
  json::Value call_tool(const json::Value& params, bool* is_error) const;

  bool allow_actions_;
  bool initialized_ = false;
  std::vector<Tool> tools_;
};

/// JSON-RPC error codes, the four this server can produce.
enum class ErrorCode : int {
  kParseError = -32700,
  kInvalidRequest = -32600,
  kMethodNotFound = -32601,
  kInvalidParams = -32602,
};

/// A JSON-RPC error response. `id` is null for a request that could not be
/// parsed far enough to have one, which is what the spec requires.
json::Value error_response(const json::Value& id, ErrorCode code,
                           const std::string& message);

/// A JSON-RPC success response.
json::Value result_response(const json::Value& id, json::Value result);

}  // namespace mpi::mcp
