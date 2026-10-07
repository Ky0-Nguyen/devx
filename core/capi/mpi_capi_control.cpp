// The DevX window's control endpoint (see core/capi/control.hpp), and the
// client `mpi mcp` uses to reach it.
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/capi/control.hpp"
#include "core/capi/mpi_capi.h"
#include "core/net/http_server.hpp"
#include "core/net/loopback_http.hpp"

namespace mpi::capi {

std::string control_file_path() {
  const char* h = std::getenv("HOME");
  return std::string(h != nullptr ? h : "") + "/Library/Application Support/DevX/control.json";
}

std::optional<ControlEndpoint> find_control_endpoint() {
  std::ifstream in(control_file_path());
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  json::ParseError perr;
  const auto doc = json::parse(ss.str(), &perr);
  if (!doc || !doc->is_object()) return std::nullopt;
  ControlEndpoint e;
  if (const auto* p = doc->find("port")) e.port = static_cast<int>(p->as_int());
  if (const auto* t = doc->find("token"); t != nullptr && t->is_string()) e.token = t->as_string();
  if (const auto* p = doc->find("pid")) e.pid = static_cast<int>(p->as_int());
  if (e.port <= 0 || e.token.empty()) return std::nullopt;
  if (e.pid > 0 && ::kill(e.pid, 0) != 0 && errno != EPERM) return std::nullopt;
  return e;
}

json::Value control_request(const std::string& path_and_query, int timeout_ms) {
  json::Value err = json::Value::object();
  const auto ep = find_control_endpoint();
  if (!ep) {
    err.set("error", json::Value::string(
                         "DevX is not running, so there is no window to show anything in. "
                         "Open DevX (it is in /Applications, or `open -a DevX`) and try again."));
    return err;
  }
  const std::string sep = path_and_query.find('?') == std::string::npos ? "?" : "&";
  const auto r = net::loopback_get(static_cast<std::uint16_t>(ep->port),
                                   path_and_query + sep + "token=" + ep->token, timeout_ms);
  if (!r.ok) {
    err.set("error", json::Value::string("the DevX window did not answer: " + r.error));
    return err;
  }
  json::ParseError perr;
  auto doc = json::parse(r.body, &perr);
  if (!doc) {
    err.set("error", json::Value::string("the DevX window answered with something that is not JSON"));
    return err;
  }
  return *doc;
}

}  // namespace mpi::capi

namespace {

using namespace mpi;

struct Pending {
  int id = 0;
  json::Value command;
  bool answered = false;
  json::Value reply;
};

struct Control {
  std::mutex mu;
  std::condition_variable cv;
  std::unique_ptr<net::HttpServer> server;
  CancellationSource cancel;
  std::thread thread;
  std::string state = "{}";
  std::deque<std::shared_ptr<Pending>> queue;   // waiting for the window to take
  std::map<int, std::shared_ptr<Pending>> taken; // taken, waiting for a reply
  int next_id = 1;
  std::string file;
};
Control g_ctl;

char* dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

net::Response show(const net::Request& req) {
  auto p = std::make_shared<Pending>();
  json::Value cmd = json::Value::object();
  for (const char* k : {"tab", "session", "issue", "device", "app", "observation", "avd", "reason"}) {
    if (req.has_param(k)) cmd.set(k, json::Value::string(req.param(k)));
  }
  p->command = std::move(cmd);
  std::unique_lock<std::mutex> lock(g_ctl.mu);
  p->id = g_ctl.next_id++;
  g_ctl.queue.push_back(p);
  // The window takes commands on its own thread a few times a second.
  const bool done = g_ctl.cv.wait_for(lock, std::chrono::seconds(5), [&] { return p->answered; });
  g_ctl.taken.erase(p->id);
  if (!done) {
    return net::Response::error(504, "the DevX window did not take the command within 5 s");
  }
  return net::Response::json(p->reply.dump(2));
}

}  // namespace

extern "C" {

char* mpi_control_start_json(const char* dir) {
  json::Value out = json::Value::object();
  std::lock_guard<std::mutex> lock(g_ctl.mu);
  if (g_ctl.server) {
    out.set("ok", json::Value::boolean(true));
    out.set("port", json::Value::integer(g_ctl.server->port()));
    return dup(out.dump());
  }
  auto server = std::make_unique<net::HttpServer>();
  server->route("GET", "/v1/state", [](const net::Request&) {
    std::lock_guard<std::mutex> l(g_ctl.mu);
    return net::Response::json(g_ctl.state);
  });
  server->route("GET", "/v1/show", show);
  std::string err;
  if (!server->listen(0, &err)) {
    out.set("ok", json::Value::boolean(false));
    out.set("error", json::Value::string(err));
    return dup(out.dump());
  }
  const std::string d = dir != nullptr && *dir ? dir : "";
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  g_ctl.file = d + "/control.json";
  json::Value f = json::Value::object();
  f.set("port", json::Value::integer(server->port()));
  f.set("token", json::Value::string(server->token()));
  f.set("pid", json::Value::integer(::getpid()));
  {
    // Owner-only before the token is written into it.
    const std::string tmp = g_ctl.file + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      const std::string body = f.dump(2);
      const ssize_t n = ::write(fd, body.data(), body.size());
      static_cast<void>(n);
      ::close(fd);
      std::rename(tmp.c_str(), g_ctl.file.c_str());
    }
  }
  g_ctl.cancel = CancellationSource();
  net::HttpServer* raw = server.get();
  const auto token = g_ctl.cancel.token();
  g_ctl.thread = std::thread([raw, token] { raw->serve(token); });
  out.set("ok", json::Value::boolean(true));
  out.set("port", json::Value::integer(server->port()));
  out.set("file", json::Value::string(g_ctl.file));
  g_ctl.server = std::move(server);
  return dup(out.dump());
}

void mpi_control_stop(void) {
  std::unique_lock<std::mutex> lock(g_ctl.mu);
  if (!g_ctl.server) return;
  g_ctl.cancel.cancel();
  lock.unlock();
  if (g_ctl.thread.joinable()) g_ctl.thread.join();
  lock.lock();
  g_ctl.server.reset();
  ::unlink(g_ctl.file.c_str());
}

void mpi_control_set_state_json(const char* json_text) {
  std::lock_guard<std::mutex> lock(g_ctl.mu);
  g_ctl.state = json_text != nullptr ? json_text : "{}";
}

char* mpi_control_next_json(void) {
  std::lock_guard<std::mutex> lock(g_ctl.mu);
  if (g_ctl.queue.empty()) return dup("{}");
  auto p = g_ctl.queue.front();
  g_ctl.queue.pop_front();
  g_ctl.taken[p->id] = p;
  json::Value o = json::Value::object();
  o.set("id", json::Value::integer(p->id));
  o.set("command", p->command);
  return dup(o.dump());
}

void mpi_control_reply(int id, const char* json_text) {
  std::lock_guard<std::mutex> lock(g_ctl.mu);
  const auto it = g_ctl.taken.find(id);
  if (it == g_ctl.taken.end()) return;
  json::ParseError perr;
  auto doc = json::parse(json_text != nullptr ? json_text : "{}", &perr);
  it->second->reply = doc ? *doc : json::Value::object();
  it->second->answered = true;
  g_ctl.cv.notify_all();
}

}  // extern "C"
