// The MCP dispatcher. Driven entirely by handing it JSON, which is the point
// of keeping the transport out of it.
//
// Hosts are unforgiving here in specific ways: a reply to a notification can
// drop the connection, an uninitialised server that answers anyway trains
// hosts to skip the handshake, and a mutating tool that runs when it was not
// meant to cannot be undone. Each of those has a test.
#include <string>

#include "core/mcp/protocol.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

mpi::json::Value parse(const std::string& text) {
  mpi::json::ParseError err;
  auto v = mpi::json::parse(text, &err);
  MPI_CHECK_MSG(v.has_value(), "the test's own JSON must parse: " + err.message);
  return *v;
}

/// A server with one of each kind of tool, so refusal and success are both
/// reachable without touching a device.
mpi::mcp::Server make(bool allow_actions, int* ran = nullptr) {
  mpi::mcp::Server s(allow_actions);
  mpi::mcp::Tool read;
  read.name = "read_thing";
  read.description = "Reads a thing.";
  read.input_schema = parse(R"({"type":"object","properties":{}})");
  read.effect = mpi::mcp::Effect::kReadOnly;
  read.run = [ran](const mpi::json::Value& args) {
    if (ran != nullptr) (*ran)++;
    const mpi::json::Value* echo = args.find("echo");
    return echo != nullptr && echo->is_string()
               ? "read:" + echo->as_string()
               : std::string("read:ok");
  };
  s.add_tool(std::move(read));

  mpi::mcp::Tool act;
  act.name = "do_thing";
  act.description = "Starts a thing.";
  act.input_schema = parse(R"({"type":"object","properties":{}})");
  act.effect = mpi::mcp::Effect::kMutating;
  act.run = [ran](const mpi::json::Value&) {
    if (ran != nullptr) (*ran)++;
    return std::string("acted");
  };
  s.add_tool(std::move(act));
  return s;
}

std::string result_text(const mpi::json::Value& response) {
  const mpi::json::Value* r = response.find("result");
  if (r == nullptr) return {};
  const mpi::json::Value* c = r->find("content");
  if (c == nullptr || !c->is_array() || c->items().empty()) return {};
  const mpi::json::Value* t = c->items().front().find("text");
  return t != nullptr && t->is_string() ? t->as_string() : std::string();
}

bool is_tool_error(const mpi::json::Value& response) {
  const mpi::json::Value* r = response.find("result");
  if (r == nullptr) return false;
  const mpi::json::Value* e = r->find("isError");
  return e != nullptr && e->as_bool();
}

mpi::json::Value initialised(mpi::mcp::Server& s) {
  auto r = s.handle(parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
  MPI_CHECK(r.has_value());
  return *r;
}

}  // namespace

MPI_TEST(initialize_answers_with_the_version_it_will_speak, {}) {
  auto s = make(false);
  const auto r = initialised(s);
  const mpi::json::Value* res = r.find("result");
  MPI_CHECK(res != nullptr);
  MPI_CHECK_MSG(res->find("protocolVersion")->as_string()
                    == mpi::mcp::kProtocolVersion,
                "the server states the revision it implements");
  MPI_CHECK_MSG(res->find("serverInfo")->find("name")->as_string()
                    == "DevX",
                "and names itself");
  MPI_CHECK_MSG(res->find("capabilities")->find("tools") != nullptr,
                "tools are advertised");
  MPI_CHECK_MSG(res->find("capabilities")->find("prompts") == nullptr,
                "and nothing this server cannot fill is advertised -- an "
                "empty prompts list is worse than no prompts capability");
  MPI_CHECK(s.initialized());
}

MPI_TEST(a_notification_is_never_answered, {}) {
  // JSON-RPC forbids replying to a notification, and a host that gets a
  // reply to its `notifications/initialized` may drop the connection.
  auto s = make(false);
  MPI_CHECK_MSG(!s.handle(parse(
                    R"({"jsonrpc":"2.0","method":"notifications/initialized"})"))
                     .has_value(),
                "initialized carries no id and gets no response");
  MPI_CHECK_MSG(s.initialized(), "but it does mark the server initialised");
  MPI_CHECK_MSG(!s.handle(parse(
                    R"({"jsonrpc":"2.0","method":"notifications/cancelled"})"))
                     .has_value(),
                "and so does any other notification");
  MPI_CHECK_MSG(!s.handle(parse(R"({"jsonrpc":"2.0","id":null,"method":"ping"})"))
                     .has_value(),
                "an explicit null id is a notification too");
}

MPI_TEST(a_call_before_initialize_is_refused, {}) {
  // A server that works without the handshake trains hosts to skip it.
  auto s = make(false);
  const auto r = s.handle(parse(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})"));
  MPI_CHECK(r.has_value());
  const mpi::json::Value* err = r->find("error");
  MPI_CHECK_MSG(err != nullptr, "it is an error, not an empty tool list");
  MPI_CHECK(err->find("code")->as_int()
            == static_cast<int>(mpi::mcp::ErrorCode::kInvalidRequest));
  MPI_CHECK_MSG(err->find("message")->as_string().find("initialize")
                    != std::string::npos,
                "and the message names what was skipped");
  MPI_CHECK_MSG(r->find("id")->as_int() == 7,
                "the id is echoed, or the host cannot match the reply");
}

MPI_TEST(ping_works_before_initialize, {}) {
  // A host may ping to see whether the process is alive before handshaking.
  auto s = make(false);
  const auto r = s.handle(parse(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
  MPI_CHECK(r.has_value() && r->find("result") != nullptr);
}

MPI_TEST(tools_list_describes_every_tool, {}) {
  auto s = make(false);
  initialised(s);
  const auto r = s.handle(parse(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"));
  const mpi::json::Value* tools = r->find("result")->find("tools");
  MPI_CHECK(tools != nullptr && tools->items().size() == 2);
  for (const auto& t : tools->items()) {
    MPI_CHECK_MSG(!t.find("name")->as_string().empty(), "every tool is named");
    MPI_CHECK_MSG(!t.find("description")->as_string().empty(),
                  "and described -- the description is all a model reads "
                  "before choosing");
    MPI_CHECK_MSG(t.find("inputSchema") != nullptr,
                  "and carries a schema, which MCP requires");
  }
}

MPI_TEST(a_read_only_server_says_a_mutating_tool_is_unavailable, {}) {
  auto s = make(false);
  initialised(s);
  const auto r = s.handle(parse(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})"));
  for (const auto& t : r->find("result")->find("tools")->items()) {
    const std::string desc = t.find("description")->as_string();
    if (t.find("name")->as_string() == "do_thing") {
      MPI_CHECK_MSG(desc.find("NOT AVAILABLE") != std::string::npos,
                    "the mutating tool's own description says so, because a "
                    "model reads prose and may not read an annotation");
      MPI_CHECK_MSG(desc.find("--allow-actions") != std::string::npos,
                    "and names the flag rather than hiding the tool");
    } else {
      MPI_CHECK_MSG(desc.find("NOT AVAILABLE") == std::string::npos,
                    "while a read-only tool is not marked");
    }
  }
}

MPI_TEST(a_mutating_tool_does_not_run_on_a_read_only_server, {}) {
  // The one failure that cannot be undone.
  int ran = 0;
  auto s = make(false, &ran);
  initialised(s);
  const auto r = s.handle(parse(
      R"({"jsonrpc":"2.0","id":5,"method":"tools/call",
          "params":{"name":"do_thing","arguments":{}}})"));
  MPI_CHECK_MSG(ran == 0, "the handler was never invoked");
  MPI_CHECK_MSG(is_tool_error(*r), "and the call is reported as an error");
  const std::string text = result_text(*r);
  MPI_CHECK_MSG(text.find("Nothing was done") != std::string::npos,
                "saying plainly that nothing happened: " + text);
  MPI_CHECK_MSG(text.find("--allow-actions") != std::string::npos,
                "and how to allow it");
}

MPI_TEST(a_mutating_tool_runs_when_actions_are_allowed, {}) {
  int ran = 0;
  auto s = make(true, &ran);
  initialised(s);
  const auto r = s.handle(parse(
      R"({"jsonrpc":"2.0","id":6,"method":"tools/call",
          "params":{"name":"do_thing","arguments":{}}})"));
  MPI_CHECK(ran == 1);
  MPI_CHECK(!is_tool_error(*r));
  MPI_CHECK(result_text(*r) == "acted");
}

MPI_TEST(a_read_only_tool_receives_its_arguments, {}) {
  auto s = make(false);
  initialised(s);
  const auto r = s.handle(parse(
      R"({"jsonrpc":"2.0","id":8,"method":"tools/call",
          "params":{"name":"read_thing","arguments":{"echo":"hi"}}})"));
  MPI_CHECK(result_text(*r) == "read:hi");
}

MPI_TEST(a_tool_error_is_not_a_protocol_error, {}) {
  // An unknown tool is a valid call whose answer is "no such tool". Returning
  // a JSON-RPC error instead would make the host retry the connection rather
  // than let the model read the answer.
  auto s = make(false);
  initialised(s);
  const auto r = s.handle(parse(
      R"({"jsonrpc":"2.0","id":9,"method":"tools/call",
          "params":{"name":"nope","arguments":{}}})"));
  MPI_CHECK_MSG(r->find("error") == nullptr, "not a JSON-RPC error");
  MPI_CHECK_MSG(is_tool_error(*r), "but a tool result marked as an error");
  MPI_CHECK(result_text(*r).find("nope") != std::string::npos);
}

MPI_TEST(malformed_calls_are_refused_with_the_right_code, {}) {
  auto s = make(false);
  initialised(s);
  const auto no_params = s.handle(parse(
      R"({"jsonrpc":"2.0","id":10,"method":"tools/call"})"));
  MPI_CHECK(no_params->find("error")->find("code")->as_int()
            == static_cast<int>(mpi::mcp::ErrorCode::kInvalidParams));
  const auto no_name = s.handle(parse(
      R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{}})"));
  MPI_CHECK(no_name->find("error")->find("code")->as_int()
            == static_cast<int>(mpi::mcp::ErrorCode::kInvalidParams));
  const auto unknown = s.handle(parse(
      R"({"jsonrpc":"2.0","id":12,"method":"resources/read"})"));
  MPI_CHECK_MSG(unknown->find("error")->find("code")->as_int()
                    == static_cast<int>(mpi::mcp::ErrorCode::kMethodNotFound),
                "a method this server does not implement is not-found, and "
                "the message lists what it does offer");
  MPI_CHECK(unknown->find("error")->find("message")->as_string()
                .find("tools/call") != std::string::npos);
}

MPI_TEST(a_batch_is_refused_rather_than_half_handled, {}) {
  auto s = make(false);
  initialised(s);
  const auto r = s.handle(parse(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])"));
  MPI_CHECK(r.has_value());
  MPI_CHECK(r->find("error")->find("message")->as_string().find("batch")
            != std::string::npos);
}

MPI_TEST(the_instructions_say_whether_this_server_can_act, {}) {
  // A host shows these to the model. Read-only has to be stated there, not
  // only discovered by a refused call.
  auto ro = make(false);
  const std::string read_only =
      initialised(ro).find("result")->find("instructions")->as_string();
  MPI_CHECK(read_only.find("refuses to change anything") != std::string::npos);
  MPI_CHECK(read_only.find("--allow-actions") != std::string::npos);

  auto rw = make(true);
  const std::string acting =
      initialised(rw).find("result")->find("instructions")->as_string();
  MPI_CHECK(acting.find("can also act") != std::string::npos);
  // Both must carry the claim the whole project rests on.
  MPI_CHECK(read_only.find("not measured") != std::string::npos);
  MPI_CHECK(acting.find("not measured") != std::string::npos);
}
