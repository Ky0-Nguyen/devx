#include "adapters/browserstack/local_tunnel.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

#include "core/util/process.hpp"

namespace mpi::browserstack {
namespace {

namespace fs = std::filesystem;

const char* kDownload =
    "https://www.browserstack.com/browserstack-local/BrowserStackLocal-darwin-x64.zip";

std::string dir() {
  const char* h = std::getenv("HOME");
  return std::string(h != nullptr ? h : "") + "/Library/Application Support/DevX/browserstack";
}

bool safe_identifier(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

LocalReply from_daemon(const proc::Result& r) {
  LocalReply out;
  if (!r.spawned) {
    out.error = r.spawn_error;
    return out;
  }
  // The daemon prints one JSON object: {"state": ..., "pid": ..., "message": ...}.
  const auto a = r.out.find('{');
  const auto b = r.out.rfind('}');
  json::ParseError perr;
  auto doc = a != std::string::npos && b != std::string::npos
                 ? json::parse(r.out.substr(a, b - a + 1), &perr)
                 : std::nullopt;
  if (!doc) {
    out.error = "BrowserStackLocal said: " + (r.out.empty() ? r.err : r.out).substr(0, 400);
    return out;
  }
  if (const auto* s = doc->find("state"); s != nullptr && s->is_string()) out.state = s->as_string();
  if (const auto* m = doc->find("message"); m != nullptr) {
    out.message = m->is_string() ? m->as_string() : m->dump();
  }
  out.ok = r.exit_code == 0;
  if (!out.ok && out.error.empty()) out.error = out.message.empty() ? "the tunnel did not start" : out.message;
  return out;
}

}  // namespace

json::Value LocalReply::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  o.set("state", json::Value::string(state));
  if (!message.empty()) o.set("message", json::Value::string(message));
  if (!error.empty()) o.set("error", json::Value::string(error));
  return o;
}

std::string local_binary_path() { return dir() + "/BrowserStackLocal"; }

bool local_binary_installed() { return ::access(local_binary_path().c_str(), X_OK) == 0; }

bool can_run_intel_binary() {
  struct utsname u {};
  ::uname(&u);
  if (std::string(u.machine) != "arm64") return true;
  proc::Options po;
  po.timeout = std::chrono::seconds(10);
  return proc::run({"/usr/bin/arch", "-x86_64", "/usr/bin/true"}, po).ok();
}

LocalReply install_local_binary(const CancellationToken& cancel) {
  LocalReply out;
  if (!can_run_intel_binary()) {
    out.error = "BrowserStack publishes BrowserStack Local for Intel Macs only, and this Mac "
                "cannot run Intel code without Rosetta 2, which is not installed. Installing "
                "it is your call: `softwareupdate --install-rosetta`.";
    return out;
  }
  std::error_code ec;
  fs::create_directories(dir(), ec);
  const std::string zip = dir() + "/BrowserStackLocal.zip";
  proc::Options po;
  po.timeout = std::chrono::minutes(5);
  po.cancel = cancel;
  auto r = proc::run({"/usr/bin/curl", "-fsSL", "-o", zip, kDownload}, po);
  if (!r.ok()) {
    out.error = "download failed: " + (r.err.empty() ? std::string("curl exited ") +
                                                           std::to_string(r.exit_code)
                                                     : r.err);
    return out;
  }
  r = proc::run({"/usr/bin/ditto", "-x", "-k", zip, dir()}, po);
  fs::remove(zip, ec);
  if (!r.ok() || !fs::exists(local_binary_path(), ec)) {
    out.error = "could not unpack the download";
    return out;
  }
  fs::permissions(local_binary_path(), fs::perms::owner_all, fs::perm_options::add, ec);
  out.ok = true;
  out.state = "installed";
  return out;
}

LocalReply start_local(const Credentials& c, const std::string& identifier) {
  LocalReply out;
  if (!c.valid()) {
    out.error = "no BrowserStack credentials";
    return out;
  }
  if (!safe_identifier(identifier)) {
    out.error = "a local identifier is letters, digits, '-' and '_'";
    return out;
  }
  if (!local_binary_installed()) {
    out.error = "BrowserStack Local is not downloaded yet";
    return out;
  }
  proc::Options po;
  po.timeout = std::chrono::seconds(90);
  return from_daemon(proc::run({local_binary_path(), "--key", c.access_key, "--local-identifier",
                                identifier, "--daemon", "start"},
                               po));
}

LocalReply stop_local(const Credentials& c, const std::string& identifier) {
  LocalReply out;
  if (!local_binary_installed() || !safe_identifier(identifier)) {
    out.error = "nothing to stop";
    return out;
  }
  proc::Options po;
  po.timeout = std::chrono::seconds(30);
  return from_daemon(proc::run({local_binary_path(), "--key", c.access_key, "--local-identifier",
                                identifier, "--daemon", "stop"},
                               po));
}

}  // namespace mpi::browserstack
