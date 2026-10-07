#include "adapters/android/emulator/emulator_process.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "adapters/android/emulator/avd.hpp"
#include "core/net/grpc_client.hpp"
#include "core/util/process.hpp"
#include "core/util/protobuf.hpp"

namespace mpi::android {
namespace {

namespace fs = std::filesystem;

std::map<std::string, std::string> read_ini(const std::string& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return out;
}

bool pid_alive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

std::string tail_of(const std::string& path, std::size_t max) {
  std::ifstream in(path, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return s.size() > max ? s.substr(s.size() - max) : s;
}

}  // namespace

json::Value RunningEmulator::to_json() const {
  json::Value o = json::Value::object();
  o.set("pid", json::Value::integer(pid));
  o.set("avd_id", json::Value::string(avd_id));
  o.set("avd_name", json::Value::string(avd_name));
  o.set("serial", json::Value::string(serial));
  o.set("grpc_port", json::Value::integer(grpc_port));
  o.set("attachable", json::Value::boolean(attachable()));
  if (jwt_only) {
    o.set("note", json::Value::string(
                      "started by Android Studio with JWT auth, which DevX cannot sign for; "
                      "use its own window, or start it from DevX"));
  }
  o.set("emulator_version", json::Value::string(emulator_version));
  return o;
}

std::string discovery_directory() {
  const char* h = std::getenv("HOME");
  return std::string(h != nullptr ? h : "") + "/Library/Caches/TemporaryItems/avd/running";
}

std::vector<RunningEmulator> running_emulators() {
  std::vector<RunningEmulator> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(discovery_directory(), ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("pid_", 0) != 0 || e.path().extension() != ".ini") continue;
    const auto kv = read_ini(e.path().string());
    RunningEmulator r;
    r.pid = std::atoi(name.c_str() + 4);
    // A stale file outlives a crashed emulator; the process is what counts.
    if (!pid_alive(r.pid)) continue;
    r.discovery_file = e.path().string();
    auto get = [&](const char* k) {
      const auto it = kv.find(k);
      return it == kv.end() ? std::string() : it->second;
    };
    r.avd_id = get("avd.id");
    r.avd_name = get("avd.name");
    const std::string console = get("port.serial");
    if (!console.empty()) r.serial = "emulator-" + console;
    r.grpc_port = std::atoi(get("grpc.port").c_str());
    r.token = get("grpc.token");
    r.jwt_only = r.token.empty() && !get("grpc.jwks").empty();
    r.emulator_version = get("emulator.version");
    out.push_back(std::move(r));
  }
  return out;
}

std::optional<RunningEmulator> find_running(const std::string& avd_id) {
  for (auto& r : running_emulators()) {
    if (r.avd_id == avd_id) return r;
  }
  return std::nullopt;
}

int free_loopback_port(int from) {
  for (int port = from; port < from + 200; port++) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(fd);
    if (ok) return port;
  }
  return 0;
}

LaunchResult launch_emulator(const std::string& sdk_root, const std::string& avd_id,
                             const LaunchOptions& o) {
  LaunchResult res;
  auto say = [&](const std::string& m) {
    if (o.progress) o.progress(m);
  };
  AvdInfo avd;
  if (!read_avd(avd_id, avd, &res.error)) return res;

  if (auto running = find_running(avd_id)) {
    res.started = true;
    res.emulator = *running;
    if (!running->attachable()) {
      res.error = avd_id + " is already running without a gRPC token DevX can use" +
                  std::string(running->jwt_only ? " (started by Android Studio)" : "") +
                  ". Stop it there and start it from DevX to see it here.";
      return res;
    }
    res.notes.push_back(avd_id + " was already running; attached to it");
    res.ok = true;
    return res;
  }

  const std::string emulator = sdk_root + "/emulator/emulator";
  if (::access(emulator.c_str(), X_OK) != 0) {
    res.error = "the Android Emulator is not installed in " + sdk_root +
                ". Install it from the SDK catalog first.";
    return res;
  }
  std::error_code ec;
  if (!fs::exists(sdk_root + "/" + avd.image_dir, ec)) {
    res.error = "the system image this AVD uses (" + avd.system_image +
                ") is not installed in " + sdk_root;
    return res;
  }
  const int port = free_loopback_port(8554);
  if (port == 0) {
    res.error = "no free loopback port for the emulator's gRPC server";
    return res;
  }
  std::vector<std::string> argv = {emulator,   "-avd",           avd_id, "-qt-hide-window",
                                   "-grpc-use-token", "-grpc", std::to_string(port),
                                   "-no-boot-anim"};
  const std::string marker = avd.directory + "/" + kColdBootMarker;
  const bool cold = o.cold_boot || fs::exists(marker, ec);
  if (cold) {
    argv.push_back("-no-snapshot-load");
    if (!o.cold_boot) res.notes.push_back("cold boot: the screen size changed since the last one");
  }
  if (o.wipe_data) argv.push_back("-wipe-data");
  res.log_path = avd.directory + "/devx-emulator.log";
  say("starting " + avd.display_name + (cold ? " (cold boot)" : ""));
  const auto spawned = proc::spawn_detached(argv, res.log_path);
  if (!spawned.spawned) {
    res.error = "could not start the emulator: " + spawned.error;
    return res;
  }
  fs::remove(marker, ec);

  // The process announces itself with a discovery file once its gRPC server
  // is up; a process that dies first wrote why into the log.
  const auto deadline = std::chrono::steady_clock::now() + o.boot_timeout;
  std::optional<RunningEmulator> found;
  while (std::chrono::steady_clock::now() < deadline && !o.cancel.cancelled()) {
    found = find_running(avd_id);
    if (found && found->attachable()) break;
    const std::string log = tail_of(res.log_path, 4000);
    if (log.find("FATAL") != std::string::npos || log.find("PANIC") != std::string::npos) {
      res.error = "the emulator failed to start:\n" + tail_of(res.log_path, 1200);
      return res;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!found || !found->attachable()) {
    res.error = o.cancel.cancelled() ? "cancelled while starting"
                                     : "the emulator did not come up; its log:\n" +
                                           tail_of(res.log_path, 1200);
    return res;
  }
  res.started = true;
  res.emulator = *found;
  say("emulator up (" + found->serial + "); waiting for Android to boot");

  net::GrpcChannel ch(static_cast<std::uint16_t>(found->grpc_port), "Bearer " + found->token);
  std::string err;
  if (!ch.connect(&err)) {
    res.error = "the emulator's gRPC server did not accept a connection: " + err;
    return res;
  }
  while (std::chrono::steady_clock::now() < deadline && !o.cancel.cancelled()) {
    std::string resp;
    const auto st = ch.unary("/android.emulation.control.EmulatorController/getStatus", "",
                             &resp, std::chrono::seconds(5));
    if (st.ok()) {
      pb::Reader r(resp);
      pb::Field f;
      while (r.next(f)) {
        if (f.number == 3 && f.value != 0) {
          res.ok = true;
          say("booted");
          return res;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  res.error = o.cancel.cancelled() ? "cancelled while booting"
                                   : "started, but Android did not finish booting within " +
                                         std::to_string(o.boot_timeout.count()) + " s";
  return res;
}

bool stop_emulator(const std::string& sdk_root, const RunningEmulator& e, std::string* error) {
  const std::string adb = sdk_root + "/platform-tools/adb";
  if (!e.serial.empty() && ::access(adb.c_str(), X_OK) == 0) {
    proc::Options po;
    po.timeout = std::chrono::seconds(10);
    // `emu kill` stops the machine at once. Android may not have written
    // recent changes to disk yet -- a settings change made seconds earlier
    // came back after the next cold boot -- so flush first.
    proc::run({adb, "-s", e.serial, "shell", "sync"}, po);
    // Some services write their state a moment after a change (a `wm size`
    // reset made and killed within the same second was back after a cold
    // boot). A short pause covers that; a change made in the last instant
    // before stopping can still be lost, as it can from Android Studio.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    proc::run({adb, "-s", e.serial, "emu", "kill"}, po);
  }
  for (int i = 0; i < 40 && pid_alive(e.pid); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (pid_alive(e.pid)) {
    ::kill(e.pid, SIGTERM);
    for (int i = 0; i < 20 && pid_alive(e.pid); i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
  if (pid_alive(e.pid)) {
    if (error != nullptr) *error = "the emulator (pid " + std::to_string(e.pid) + ") did not stop";
    return false;
  }
  return true;
}

}  // namespace mpi::android
