#include "core/mcp/protocol.hpp"

namespace mpi::mcp {
namespace {

const json::Value* field(const json::Value& v, const char* key) {
  return v.is_object() ? v.find(key) : nullptr;
}

std::string str_at(const json::Value& v, const char* key) {
  const json::Value* f = field(v, key);
  return f != nullptr && f->is_string() ? f->as_string() : std::string();
}

/// One `content` entry, which is how MCP carries every tool answer.
json::Value text_content(const std::string& text) {
  json::Value one = json::Value::object();
  one.set("type", json::Value::string("text"));
  one.set("text", json::Value::string(text));
  json::Value arr = json::Value::array();
  arr.push_back(std::move(one));
  return arr;
}

}  // namespace

json::Value error_response(const json::Value& id, ErrorCode code,
                           const std::string& message) {
  json::Value err = json::Value::object();
  err.set("code", json::Value::integer(static_cast<int>(code)));
  err.set("message", json::Value::string(message));
  json::Value out = json::Value::object();
  out.set("jsonrpc", json::Value::string("2.0"));
  out.set("id", id);
  out.set("error", std::move(err));
  return out;
}

json::Value result_response(const json::Value& id, json::Value result) {
  json::Value out = json::Value::object();
  out.set("jsonrpc", json::Value::string("2.0"));
  out.set("id", id);
  out.set("result", std::move(result));
  return out;
}

void Server::add_tool(Tool tool) { tools_.push_back(std::move(tool)); }

json::Value Server::initialize_result() const {
  json::Value tools_cap = json::Value::object();
  // No list-changed notifications: the catalogue is fixed at start-up, so
  // claiming the capability would promise a message that never comes.
  tools_cap.set("listChanged", json::Value::boolean(false));

  json::Value caps = json::Value::object();
  caps.set("tools", std::move(tools_cap));

  json::Value info = json::Value::object();
  info.set("name", json::Value::string("DevX"));
  info.set("version", json::Value::string("0.2.0"));

  json::Value out = json::Value::object();
  out.set("protocolVersion", json::Value::string(kProtocolVersion));
  out.set("capabilities", std::move(caps));
  out.set("serverInfo", std::move(info));
  // Said once, where a host will show it: the difference between this server
  // reading and this server acting.
  out.set("instructions", json::Value::string(
      allow_actions_
          ? "Reads captures and can also act: record, boot a device, observe "
            "a running app. Every number it returns was measured or is "
            "marked as not measured; nothing is estimated. A missing value is "
            "absent rather than zero, and that distinction is load-bearing."
          : "Reads captures, devices and analysis, and refuses to change "
            "anything: recording, booting and editing suppressions are "
            "available only when this server is started with --allow-actions. "
            "Every number it returns was measured or is marked as not "
            "measured; nothing is estimated."));
  return out;
}

json::Value Server::tools_list_result() const {
  json::Value arr = json::Value::array();
  for (const Tool& t : tools_) {
    json::Value one = json::Value::object();
    one.set("name", json::Value::string(t.name));
    // The effect is in the description as well as the schema, because a
    // model reads prose and may not read an annotation.
    one.set("description",
            json::Value::string(
                t.effect == Effect::kMutating && !allow_actions_
                    ? t.description +
                          " NOT AVAILABLE: this server was started read-only; "
                          "restart it with --allow-actions to enable this."
                    : t.description));
    one.set("inputSchema", t.input_schema);
    arr.push_back(std::move(one));
  }
  json::Value out = json::Value::object();
  out.set("tools", std::move(arr));
  return out;
}

json::Value Server::call_tool(const json::Value& params, bool* is_error) const {
  *is_error = false;
  const std::string name = str_at(params, "name");
  const json::Value* args = field(params, "arguments");
  const json::Value empty = json::Value::object();

  for (const Tool& t : tools_) {
    if (t.name != name) continue;
    if (t.effect == Effect::kMutating && !allow_actions_) {
      *is_error = true;
      json::Value out = json::Value::object();
      out.set("content", text_content(
          "refused: '" + name + "' changes something -- it starts a process, "
          "changes this machine's state, or writes a file -- and this server "
          "was started read-only. Restart it with --allow-actions if that is "
          "what you want. Nothing was done."));
      out.set("isError", json::Value::boolean(true));
      return out;
    }
    const std::string text = t.run(args != nullptr ? *args : empty);
    json::Value out = json::Value::object();
    out.set("content", text_content(text));
    return out;
  }

  *is_error = true;
  json::Value out = json::Value::object();
  out.set("content", text_content("no tool named '" + name + "'"));
  out.set("isError", json::Value::boolean(true));
  return out;
}

std::optional<json::Value> Server::handle(const json::Value& message) {
  if (message.is_array()) {
    // The spec allows a batch; no host sends one to a stdio server, and
    // half-handling it would be worse than saying so.
    return error_response(json::Value::null(), ErrorCode::kInvalidRequest,
                          "batched requests are not supported by this server");
  }
  if (!message.is_object()) {
    return error_response(json::Value::null(), ErrorCode::kInvalidRequest,
                          "a JSON-RPC message must be an object");
  }

  const std::string method = str_at(message, "method");
  const json::Value* id_field = field(message, "id");
  // No id means a notification: JSON-RPC forbids a reply, and a host that
  // gets one for `notifications/initialized` may drop the connection.
  const bool is_notification = id_field == nullptr || id_field->is_null();
  const json::Value id = id_field != nullptr ? *id_field : json::Value::null();

  if (method.empty()) {
    if (is_notification) return std::nullopt;
    return error_response(id, ErrorCode::kInvalidRequest,
                          "no method named in the request");
  }

  if (method == "initialize") {
    initialized_ = true;
    if (is_notification) return std::nullopt;
    return result_response(id, initialize_result());
  }

  // Notifications this server accepts and answers nothing to.
  if (method == "notifications/initialized" || method == "initialized") {
    initialized_ = true;
    return std::nullopt;
  }
  if (method.rfind("notifications/", 0) == 0) return std::nullopt;

  if (is_notification) return std::nullopt;

  if (method == "ping") return result_response(id, json::Value::object());

  if (!initialized_) {
    // A server that works without initialising trains hosts to skip it.
    return error_response(id, ErrorCode::kInvalidRequest,
                          "initialize must be called before '" + method + "'");
  }

  if (method == "tools/list") return result_response(id, tools_list_result());

  if (method == "tools/call") {
    const json::Value* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      return error_response(id, ErrorCode::kInvalidParams,
                            "tools/call needs a params object");
    }
    if (str_at(*params, "name").empty()) {
      return error_response(id, ErrorCode::kInvalidParams,
                            "tools/call needs a tool name");
    }
    bool is_error = false;
    return result_response(id, call_tool(*params, &is_error));
  }

  return error_response(id, ErrorCode::kMethodNotFound,
                        "this server does not implement '" + method +
                            "'. It offers initialize, ping, tools/list and "
                            "tools/call.");
}

}  // namespace mpi::mcp
