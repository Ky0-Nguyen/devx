#include "adapters/ios/view_probe_client.hpp"

#include <mach-o/dyld.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include "core/util/process.hpp"

namespace mpi::ios {
namespace {

std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

bool is_file(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string executable_dir() {
  char buf[PATH_MAX];
  std::uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return {};
  char real[PATH_MAX];
  if (::realpath(buf, real) == nullptr) return {};
  std::string p(real);
  const auto slash = p.rfind('/');
  return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

}  // namespace

std::string probe_socket_path(const std::string& udid,
                              const std::string& bundle_id) {
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(fnv1a(udid + "|" + bundle_id)));
  return std::string("/tmp/devx-probe-") + hex + ".sock";
}

std::optional<std::string> find_view_probe(std::vector<std::string>* searched) {
  std::vector<std::string> candidates;
  if (const char* env = std::getenv("MPI_VIEW_PROBE"); env != nullptr && *env) {
    candidates.emplace_back(env);
  }
  const std::string dir = executable_dir();
  if (!dir.empty()) {
    candidates.push_back(dir + "/libdevx_view_probe.dylib");
    candidates.push_back(dir + "/../Resources/libdevx_view_probe.dylib");
  }
  for (const auto& c : candidates) {
    if (searched != nullptr) searched->push_back(c);
    if (is_file(c)) {
      char real[PATH_MAX];
      return ::realpath(c.c_str(), real) != nullptr ? std::string(real) : c;
    }
  }
  return std::nullopt;
}

ProbeLaunch launch_with_probe(const std::string& udid,
                              const std::string& bundle_id,
                              const std::string& probe_path,
                              const std::string& socket_path,
                              std::chrono::milliseconds timeout,
                              const CancellationToken& cancel) {
  ProbeLaunch res;
  if (!proc::is_safe_argument(udid, true) || !proc::is_safe_argument(bundle_id, true)) {
    res.error = "refusing an identifier that would be read as an option";
    return res;
  }
  // A socket left by an earlier probe would answer nothing and could be
  // mistaken for one still loading.
  ::unlink(socket_path.c_str());
  proc::Options po;
  po.timeout = timeout;
  po.cancel = cancel;
  po.env_overrides = {"SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=" + probe_path,
                      "SIMCTL_CHILD_DEVX_PROBE_SOCKET=" + socket_path};
  const auto r = proc::run({"/usr/bin/xcrun", "simctl", "launch",
                            "--terminate-running-process", udid, bundle_id},
                           po);
  if (!r.ok()) {
    res.error = r.spawned ? (r.timed_out ? "simctl launch did not finish in time"
                                         : trim(r.err.empty() ? r.out : r.err))
                          : r.spawn_error;
    return res;
  }
  // "host.exp.Exponent: 66075"
  const std::string out = trim(r.out);
  const auto colon = out.rfind(':');
  if (colon != std::string::npos) {
    res.pid = std::atoll(out.c_str() + colon + 1);
  }
  res.launched = res.pid > 0;
  if (!res.launched) res.error = "simctl launch printed no pid: " + out;
  return res;
}

const char* to_string(ProbeAnswer a) {
  switch (a) {
    case ProbeAnswer::kAnswered:  return "answered";
    case ProbeAnswer::kNotLoaded: return "not_loaded";
    case ProbeAnswer::kTimedOut:  return "timed_out";
    case ProbeAnswer::kFailed:    return "failed";
  }
  return "failed";
}

ProbeFetch fetch_probe_snapshot(const std::string& socket_path,
                                std::chrono::milliseconds timeout,
                                bool wait_for_listener,
                                const CancellationToken& cancel) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + timeout;
  ProbeFetch res;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof addr.sun_path) {
    res.error = "socket path too long: " + socket_path;
    return res;
  }
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

  int fd = -1;
  for (;;) {
    if (cancel.cancelled()) {
      res.error = "cancelled";
      return res;
    }
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      res.error = std::string("socket: ") + std::strerror(errno);
      return res;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) break;
    const int err = errno;
    ::close(fd);
    fd = -1;
    const bool nobody = err == ENOENT || err == ECONNREFUSED;
    if (!nobody) {
      res.error = std::string("connect: ") + std::strerror(err);
      return res;
    }
    if (!wait_for_listener || clock::now() >= deadline) {
      res.answer = wait_for_listener ? ProbeAnswer::kTimedOut : ProbeAnswer::kNotLoaded;
      res.error = wait_for_listener
                      ? "the probe never started listening; the app may have "
                        "failed to launch, or refused the injected library"
                      : "nothing is listening at " + socket_path;
      return res;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  constexpr std::size_t kMaxBytes = 256u * 1024u * 1024u;
  char buf[65536];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - clock::now());
    if (left.count() <= 0 || cancel.cancelled()) {
      ::close(fd);
      res.answer = ProbeAnswer::kTimedOut;
      res.error = cancel.cancelled()
                      ? "cancelled"
                      : "the probe accepted the connection but did not finish "
                        "answering in time: the app's main thread is busy, "
                        "and the tree is read there";
      res.body.clear();
      return res;
    }
    pollfd p{fd, POLLIN, 0};
    const int slice = static_cast<int>(std::min<long long>(left.count(), 200));
    const int ready = ::poll(&p, 1, slice);
    if (ready < 0 && errno != EINTR) break;
    if (ready <= 0) continue;
    const ssize_t got = ::read(fd, buf, sizeof buf);
    if (got < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (got == 0) {
      ::close(fd);
      res.answer = res.body.empty() ? ProbeAnswer::kFailed : ProbeAnswer::kAnswered;
      if (res.body.empty()) res.error = "the probe closed the connection without answering";
      return res;
    }
    res.body.append(buf, static_cast<std::size_t>(got));
    if (res.body.size() > kMaxBytes) {
      ::close(fd);
      res.body.clear();
      res.error = "the probe's answer exceeded 256 MiB and was refused";
      return res;
    }
  }
  ::close(fd);
  res.error = std::string("read: ") + std::strerror(errno);
  return res;
}

std::map<std::string, std::string> demangle_swift_names(
    const std::vector<std::string>& names, std::chrono::milliseconds timeout) {
  std::map<std::string, std::string> out;
  if (names.empty()) return out;
  std::vector<std::string> argv = {"/usr/bin/xcrun", "swift-demangle",
                                   "--simplified", "--compact"};
  for (const auto& n : names) {
    // A runtime class name never starts with '-'; anything that does is not
    // one and must not reach argv as an option.
    if (!n.empty() && n[0] != '-') argv.push_back(n);
  }
  proc::Options po;
  po.timeout = timeout;
  const auto r = proc::run(argv, po);
  if (!r.ok()) return out;
  std::istringstream in(r.out);
  std::string line;
  std::size_t i = 4;
  while (std::getline(in, line) && i < argv.size()) {
    out[argv[i]] = trim(line);
    i++;
  }
  return out;
}

}  // namespace mpi::ios
