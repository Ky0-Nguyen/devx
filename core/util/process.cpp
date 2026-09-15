#include "core/util/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern char** environ;

namespace mpi::proc {
namespace {

class PipePair {
 public:
  bool open() {
    if (::pipe(fds_) != 0) return false;
    // Non-blocking reads so a stalled child cannot wedge the poll loop.
    ::fcntl(fds_[0], F_SETFL, O_NONBLOCK);
    return true;
  }
  int read_end() const { return fds_[0]; }
  int write_end() const { return fds_[1]; }
  void close_read() {
    if (fds_[0] >= 0) {
      ::close(fds_[0]);
      fds_[0] = -1;
    }
  }
  void close_write() {
    if (fds_[1] >= 0) {
      ::close(fds_[1]);
      fds_[1] = -1;
    }
  }
  ~PipePair() {
    close_read();
    close_write();
  }

 private:
  int fds_[2] = {-1, -1};
};

std::chrono::steady_clock::time_point now() {
  return std::chrono::steady_clock::now();
}

}  // namespace

bool is_safe_argument(const std::string& arg, bool reject_option_like) {
  if (arg.find('\0') != std::string::npos) return false;
  if (reject_option_like && !arg.empty() && arg[0] == '-') return false;
  return true;
}

DetachedResult spawn_detached(const std::vector<std::string>& argv) {
  DetachedResult res;
  if (argv.empty()) {
    res.error = "no program to run";
    return res;
  }

  const pid_t first = ::fork();
  if (first < 0) {
    res.error = "fork failed";
    return res;
  }
  if (first == 0) {
    // Intermediate child: leave the parent's session so the grandchild is not
    // killed when the caller's terminal goes away, then fork again so the
    // grandchild is reparented to init and nobody has to reap it.
    ::setsid();
    const pid_t second = ::fork();
    if (second < 0) ::_exit(127);
    if (second > 0) ::_exit(0);

    // Grandchild: detach from the caller's stdio. A long-running emulator
    // writing into a pipe nobody drains would eventually block on a full
    // buffer, which would look like the emulator hanging.
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) ::close(devnull);
    }
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);
    ::execvp(raw[0], raw.data());
    ::_exit(127);
  }

  // Parent: reap the intermediate child immediately. It has already exited or
  // is about to, so this does not wait on the program itself.
  int status = 0;
  ::waitpid(first, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    res.error = "the program could not be started (exec failed)";
    return res;
  }
  res.spawned = true;
  return res;
}

std::optional<std::string> which(const std::string& exe) {
  if (exe.find('/') != std::string::npos) {
    return ::access(exe.c_str(), X_OK) == 0 ? std::optional<std::string>(exe)
                                            : std::nullopt;
  }
  const char* path = std::getenv("PATH");
  if (!path) return std::nullopt;
  std::istringstream ss(path);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) continue;
    std::string candidate = dir + "/" + exe;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return std::nullopt;
}

Result run(const std::vector<std::string>& argv, const Options& opts) {
  Result r;
  const auto started = now();
  if (argv.empty()) {
    r.spawn_error = "empty argv";
    return r;
  }
  for (const auto& a : argv) {
    if (!is_safe_argument(a, /*reject_option_like=*/false)) {
      r.spawn_error = "argument contains NUL";
      return r;
    }
  }

  PipePair out_pipe;
  PipePair err_pipe;
  if (!out_pipe.open() || !err_pipe.open()) {
    r.spawn_error = std::string("pipe failed: ") + std::strerror(errno);
    return r;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe.write_end(),
                                   STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, err_pipe.write_end(),
                                   STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, out_pipe.read_end());
  posix_spawn_file_actions_addclose(&actions, err_pipe.read_end());

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  std::vector<std::string> env_storage;
  std::vector<char*> cenv;
  char** env_ptr = environ;
  if (!opts.env_overrides.empty()) {
    for (char** e = environ; e && *e; ++e) {
      const std::string entry(*e);
      const std::size_t eq = entry.find('=');
      const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
      bool overridden = false;
      for (const auto& ov : opts.env_overrides) {
        const std::size_t oeq = ov.find('=');
        if (oeq != std::string::npos && ov.compare(0, oeq, key) == 0 &&
            key.size() == oeq) {
          overridden = true;
          break;
        }
      }
      if (!overridden) env_storage.push_back(entry);
    }
    for (const auto& ov : opts.env_overrides) env_storage.push_back(ov);
    cenv.reserve(env_storage.size() + 1);
    for (auto& s : env_storage) cenv.push_back(const_cast<char*>(s.c_str()));
    cenv.push_back(nullptr);
    env_ptr = cenv.data();
  }

  pid_t pid = -1;
  const int spawn_rc =
      posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, cargv.data(), env_ptr);
  posix_spawn_file_actions_destroy(&actions);
  out_pipe.close_write();
  err_pipe.close_write();

  if (spawn_rc != 0) {
    r.spawn_error = argv[0] + ": " + std::strerror(spawn_rc);
    r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(now() - started);
    return r;
  }
  r.spawned = true;

  bool out_open = true;
  bool err_open = true;
  bool over_limit = false;
  char buf[16384];

  while (out_open || err_open) {
    if (opts.cancel.cancelled()) {
      r.cancelled = true;
      ::kill(pid, opts.stop_signal);
      break;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now() - started);
    if (opts.timeout.count() > 0 && elapsed >= opts.timeout) {
      r.timed_out = true;
      ::kill(pid, opts.stop_signal);
      break;
    }

    struct pollfd pfds[2];
    int nfds = 0;
    int out_idx = -1;
    int err_idx = -1;
    if (out_open) {
      pfds[nfds] = {out_pipe.read_end(), POLLIN, 0};
      out_idx = nfds++;
    }
    if (err_open) {
      pfds[nfds] = {err_pipe.read_end(), POLLIN, 0};
      err_idx = nfds++;
    }

    int wait_ms = 200;
    if (opts.timeout.count() > 0) {
      const auto remaining = opts.timeout - elapsed;
      wait_ms = static_cast<int>(remaining.count());
      if (wait_ms > 200) wait_ms = 200;
      if (wait_ms < 0) wait_ms = 0;
    }
    const int prc = ::poll(pfds, static_cast<nfds_t>(nfds), wait_ms);
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (prc == 0) continue;

    auto drain = [&](int idx, int fd, bool& open_flag, std::string& sink) {
      if (idx < 0) return;
      if ((pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) == 0) return;
      for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
          if (sink.size() + static_cast<std::size_t>(n) > opts.max_output_bytes) {
            over_limit = true;
            open_flag = false;
            return;
          }
          sink.append(buf, static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          open_flag = false;
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        open_flag = false;
        return;
      }
    };
    drain(out_idx, out_pipe.read_end(), out_open, r.out);
    drain(err_idx, err_pipe.read_end(), err_open, r.err);
    if (over_limit) {
      ::kill(pid, SIGTERM);
      break;
    }
  }

  int status = 0;
  // Reap. If we signalled the child, give it a brief window then SIGKILL so a
  // cancelled or timed-out capture cannot leave an orphan collector behind.
  if (r.timed_out || r.cancelled || over_limit) {
    // Poll in 20 ms steps for the caller's grace period, SIGKILL at the end
    // of it. A child that writes a file on shutdown needs the whole window;
    // one that does not is reaped on the first poll either way.
    const long steps =
        opts.stop_grace.count() > 0 ? opts.stop_grace.count() / 20 : 25;
    for (long i = 0; i <= steps + 5; ++i) {
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) break;
      struct timespec ts {0, 20 * 1000 * 1000};
      ::nanosleep(&ts, nullptr);
      if (i == steps) ::kill(pid, SIGKILL);
    }
  } else {
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  }
  if (over_limit) {
    r.err += "\n[mpi] output exceeded max_output_bytes; child terminated";
  }
  r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(now() - started);
  return r;
}

}  // namespace mpi::proc
