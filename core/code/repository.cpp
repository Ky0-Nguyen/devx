#include "core/code/repository.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "core/symbols/symbol_service.hpp"
#include "core/util/process.hpp"

namespace mpi::code {
namespace {

namespace fs = std::filesystem;

struct GitOut {
  bool ok = false;
  std::string out;
  std::string error;
};

GitOut git(const std::string& root, std::vector<std::string> args,
           std::size_t max_bytes = 4 * 1024 * 1024) {
  GitOut r;
  std::vector<std::string> argv = {"/usr/bin/git", "-C", root, "--no-pager",
                                   "-c", "core.quotepath=off"};
  argv.insert(argv.end(), args.begin(), args.end());
  proc::Options po;
  po.timeout = std::chrono::seconds(20);
  po.max_output_bytes = max_bytes;
  // A repository's own config can name programs (diff drivers, textconv);
  // none of them run here.
  po.env_overrides = {"GIT_CONFIG_NOSYSTEM=1", "GIT_TERMINAL_PROMPT=0", "GIT_EXTERNAL_DIFF="};
  const auto res = proc::run(argv, po);
  if (!res.spawned) {
    r.error = "git is not available: " + res.spawn_error;
    return r;
  }
  if (res.timed_out) {
    r.error = "git did not answer within 20 s";
    return r;
  }
  if (res.exit_code != 0) {
    std::string e = res.err;
    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
    r.error = e.empty() ? "git failed" : e;
    return r;
  }
  r.ok = true;
  r.out = res.out;
  return r;
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  return s.substr(i);
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

// https://user:token@host/path -> https://host/path
std::string strip_credentials(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  const auto at = url.find('@', scheme + 3);
  const auto slash = url.find('/', scheme + 3);
  if (at == std::string::npos || (slash != std::string::npos && at > slash)) return url;
  return url.substr(0, scheme + 3) + url.substr(at + 1);
}

bool path_is_safe(const std::string& p) {
  if (p.empty() || p[0] == '-' || p[0] == '/' || p.find("..") != std::string::npos) return false;
  return std::none_of(p.begin(), p.end(), [](char c) { return c == '\0' || c == '\n' || c == '\r'; });
}

}  // namespace

bool ref_is_safe(const std::string& ref) {
  if (ref.empty() || ref.size() > 200 || ref[0] == '-' || ref.find("..") != std::string::npos) {
    return false;
  }
  return std::none_of(ref.begin(), ref.end(), [](char c) {
    return std::iscntrl(static_cast<unsigned char>(c)) || std::isspace(static_cast<unsigned char>(c)) ||
           c == '~' || c == '^' || c == ':' || c == '\\';
  });
}

json::Value RepositoryInfo::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  o.set("root", json::Value::string(root));
  if (!head.empty()) o.set("head", json::Value::string(head));
  if (!branch.empty()) o.set("branch", json::Value::string(branch));
  if (!remote_url.empty()) o.set("remote_url", json::Value::string(remote_url));
  return o;
}

json::Value CommitInfo::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!ok) return o;
  o.set("sha", json::Value::string(sha));
  o.set("author", json::Value::string(author));
  o.set("authored_at", json::Value::string(authored_at));
  o.set("subject", json::Value::string(subject));
  json::Value p = json::Value::array();
  for (const auto& x : parents) p.push_back(json::Value::string(x));
  o.set("parents", std::move(p));
  return o;
}

json::Value DiffSummary::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!ok) return o;
  o.set("base", json::Value::string(base));
  o.set("head", json::Value::string(head));
  json::Value fs_ = json::Value::array();
  for (const auto& f : files) {
    json::Value one = json::Value::object();
    one.set("status", json::Value::string(f.status));
    one.set("path", json::Value::string(f.path));
    if (f.added >= 0) one.set("added", json::Value::integer(f.added));
    if (f.removed >= 0) one.set("removed", json::Value::integer(f.removed));
    fs_.push_back(std::move(one));
  }
  o.set("files", std::move(fs_));
  o.set("patch", json::Value::string(patch));
  o.set("patch_truncated", json::Value::boolean(patch_truncated));
  return o;
}

json::Value SourceFile::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!ok) return o;
  o.set("path", json::Value::string(path));
  o.set("ref", json::Value::string(ref));
  o.set("text", json::Value::string(text));
  o.set("truncated", json::Value::boolean(truncated));
  return o;
}

json::Value BlameInfo::to_json() const {
  json::Value o = json::Value::object();
  o.set("ok", json::Value::boolean(ok));
  if (!error.empty()) o.set("error", json::Value::string(error));
  if (!ok) return o;
  o.set("path", json::Value::string(path));
  o.set("line", json::Value::integer(line));
  o.set("commit", json::Value::string(commit));
  o.set("author", json::Value::string(author));
  o.set("authored_at", json::Value::string(authored_at));
  o.set("summary", json::Value::string(summary));
  o.set("text", json::Value::string(text));
  return o;
}

GitRepository::GitRepository(std::string root) : root_(std::move(root)) {}

RepositoryInfo GitRepository::repository() const {
  RepositoryInfo r;
  r.root = root_;
  std::error_code ec;
  if (root_.empty() || !fs::is_directory(root_, ec)) {
    r.error = root_.empty() ? "no repository configured for this workspace"
                            : "the repository is not at " + root_ + " (moved or deleted?)";
    return r;
  }
  const auto top = git(root_, {"rev-parse", "--show-toplevel"});
  if (!top.ok) {
    r.error = root_ + " is not a git repository: " + top.error;
    return r;
  }
  const auto head = git(root_, {"rev-parse", "HEAD"});
  if (head.ok) r.head = trim(head.out);
  const auto branch = git(root_, {"symbolic-ref", "--quiet", "--short", "HEAD"});
  if (branch.ok) r.branch = trim(branch.out);
  const auto remote = git(root_, {"config", "--get", "remote.origin.url"});
  if (remote.ok) r.remote_url = strip_credentials(trim(remote.out));
  r.ok = true;
  return r;
}

CommitInfo GitRepository::commit(const std::string& ref) const {
  CommitInfo c;
  if (!ref_is_safe(ref)) {
    c.error = "not a commit reference: '" + ref + "'";
    return c;
  }
  // %x1f between fields: a subject can hold anything but a unit separator.
  const auto r = git(root_, {"show", "-s", "--format=%H%x1f%an%x1f%aI%x1f%P%x1f%s", ref + "^{commit}", "--"});
  if (!r.ok) {
    c.error = "commit " + ref + " is not in " + root_ + ": " + r.error;
    return c;
  }
  std::vector<std::string> f;
  std::string cur;
  for (char ch : trim(r.out)) {
    if (ch == '\x1f') {
      f.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  f.push_back(cur);
  if (f.size() < 5) {
    c.error = "unexpected git output";
    return c;
  }
  c.ok = true;
  c.sha = f[0];
  c.author = f[1];
  c.authored_at = f[2];
  std::istringstream ps(f[3]);
  std::string p;
  while (ps >> p) c.parents.push_back(p);
  c.subject = f[4];
  return c;
}

DiffSummary GitRepository::diff(const std::string& base_in, const std::string& head,
                                std::size_t max_patch_bytes,
                                const std::vector<std::string>& paths) const {
  DiffSummary d;
  std::string base = base_in;
  if (!ref_is_safe(head) || (!base.empty() && !ref_is_safe(base))) {
    d.error = "not a commit reference";
    return d;
  }
  if (base.empty()) {
    const auto c = commit(head);
    if (!c.ok) {
      d.error = c.error;
      return d;
    }
    if (c.parents.empty()) {
      d.error = head + " has no parent to compare with";
      return d;
    }
    base = c.parents[0];
  }
  for (const auto& p : paths) {
    if (!path_is_safe(p)) {
      d.error = "not a repository path: '" + p + "'";
      return d;
    }
  }
  d.base = base;
  d.head = head;
  std::vector<std::string> spec = {base, head, "--"};
  spec.insert(spec.end(), paths.begin(), paths.end());
  std::vector<std::string> numstat = {"diff", "--no-ext-diff", "--no-textconv", "--numstat", "-M"};
  numstat.insert(numstat.end(), spec.begin(), spec.end());
  const auto ns = git(root_, numstat);
  if (!ns.ok) {
    d.error = ns.error;
    return d;
  }
  std::vector<std::string> names = {"diff", "--no-ext-diff", "--no-textconv", "--name-status", "-M"};
  names.insert(names.end(), spec.begin(), spec.end());
  const auto nm = git(root_, names);
  std::vector<std::string> status_lines = nm.ok ? split_lines(nm.out) : std::vector<std::string>{};
  std::size_t i = 0;
  for (const auto& line : split_lines(ns.out)) {
    std::istringstream in(line);
    std::string a, r, path;
    if (!std::getline(in, a, '\t') || !std::getline(in, r, '\t') || !std::getline(in, path)) continue;
    FileChange f;
    f.path = path;
    f.added = a == "-" ? -1 : std::atoi(a.c_str());
    f.removed = r == "-" ? -1 : std::atoi(r.c_str());
    f.status = i < status_lines.size() && !status_lines[i].empty() ? status_lines[i].substr(0, 1) : "M";
    d.files.push_back(std::move(f));
    i++;
    if (d.files.size() >= 500) break;
  }
  std::vector<std::string> patch = {"diff", "--no-ext-diff", "--no-textconv", "-U3", "-M"};
  patch.insert(patch.end(), spec.begin(), spec.end());
  // No patch asked for: not computed at all.
  const auto pt = max_patch_bytes == 0 ? GitOut{} : git(root_, patch, std::max<std::size_t>(max_patch_bytes + 1, 64 * 1024));
  if (pt.ok) {
    d.patch = pt.out;
    if (d.patch.size() > max_patch_bytes) {
      d.patch.resize(max_patch_bytes);
      d.patch_truncated = true;
    }
  }
  d.ok = true;
  return d;
}

SourceFile GitRepository::read_file(const std::string& path, const std::string& ref_in,
                                    std::size_t max_bytes) const {
  SourceFile s;
  const std::string ref = ref_in.empty() ? "HEAD" : ref_in;
  s.path = path;
  s.ref = ref;
  if (!path_is_safe(path) || !ref_is_safe(ref)) {
    s.error = "not a repository path or ref";
    return s;
  }
  // The defence the symbol service already uses, before anything is read.
  if (!symbols::path_is_within_root(root_, root_ + "/" + path)) {
    s.error = path + " is outside the repository";
    return s;
  }
  const auto r = git(root_, {"show", ref + ":" + path}, max_bytes + 1);
  if (!r.ok) {
    s.error = path + " at " + ref + ": " + r.error;
    return s;
  }
  s.ok = true;
  s.text = r.out;
  if (s.text.size() > max_bytes) {
    s.text.resize(max_bytes);
    s.truncated = true;
  }
  return s;
}

SourceFile GitRepository::lines(const std::string& path, const std::string& ref, int first,
                                int last) const {
  SourceFile f = read_file(path, ref, 4 * 1024 * 1024);
  if (!f.ok) return f;
  first = std::max(1, first);
  last = std::max(first, last);
  std::string out;
  int n = 0;
  for (const auto& line : split_lines(f.text)) {
    n++;
    if (n < first) continue;
    if (n > last) break;
    out += std::to_string(n) + "  " + line + "\n";
  }
  f.text = out;
  f.truncated = false;
  return f;
}

BlameInfo GitRepository::blame(const std::string& path, int line, const std::string& ref_in) const {
  BlameInfo b;
  b.path = path;
  b.line = line;
  const std::string ref = ref_in.empty() ? "HEAD" : ref_in;
  if (!path_is_safe(path) || !ref_is_safe(ref) || line < 1) {
    b.error = "not a repository path, ref or line";
    return b;
  }
  if (!symbols::path_is_within_root(root_, root_ + "/" + path)) {
    b.error = path + " is outside the repository";
    return b;
  }
  const std::string range = std::to_string(line) + "," + std::to_string(line);
  const auto r = git(root_, {"blame", "--porcelain", "-L", range, ref, "--", path});
  if (!r.ok) {
    b.error = r.error;
    return b;
  }
  for (const auto& l : split_lines(r.out)) {
    if (b.commit.empty() && l.size() >= 40) {
      b.commit = l.substr(0, 40);
    } else if (l.rfind("author ", 0) == 0) {
      b.author = l.substr(7);
    } else if (l.rfind("author-time ", 0) == 0) {
      const std::time_t t = static_cast<std::time_t>(std::atoll(l.c_str() + 12));
      std::tm tm{};
      ::gmtime_r(&t, &tm);
      char buf[32];
      std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
      b.authored_at = buf;
    } else if (l.rfind("summary ", 0) == 0) {
      b.summary = l.substr(8);
    } else if (!l.empty() && l[0] == '\t') {
      b.text = l.substr(1);
    }
  }
  b.ok = !b.commit.empty();
  if (!b.ok) b.error = "git blame gave no commit for that line";
  return b;
}

std::optional<std::string> GitRepository::resolve_path(const std::string& frame_file) const {
  if (frame_file.empty() || frame_file.size() > 1024) return std::nullopt;
  if (!tracked_loaded_) {
    tracked_loaded_ = true;
    const auto r = git(root_, {"ls-files", "-z"}, 64 * 1024 * 1024);
    if (r.ok) {
      std::string cur;
      for (char c : r.out) {
        if (c == '\0') {
          if (!cur.empty()) tracked_.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
    }
  }
  // Normalise: webpack://, file://, app:///, leading ./ and backslashes.
  std::string f = frame_file;
  for (const char* prefix : {"webpack:///", "webpack://", "file://", "app:///", "app://"}) {
    if (f.rfind(prefix, 0) == 0) f = f.substr(std::string(prefix).size());
  }
  std::replace(f.begin(), f.end(), '\\', '/');
  while (f.rfind("./", 0) == 0) f = f.substr(2);
  if (f.empty()) return std::nullopt;
  // The longest suffix of the frame's path that names exactly one tracked file.
  std::vector<std::string> parts;
  std::string cur;
  for (char c : f) {
    if (c == '/') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  for (std::size_t start = 0; start < parts.size(); start++) {
    std::string suffix;
    for (std::size_t i = start; i < parts.size(); i++) suffix += (suffix.empty() ? "" : "/") + parts[i];
    std::vector<const std::string*> hits;
    for (const auto& t : tracked_) {
      if (t == suffix || (t.size() > suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                          t[t.size() - suffix.size() - 1] == '/')) {
        hits.push_back(&t);
        if (hits.size() > 1) break;
      }
    }
    if (hits.size() == 1) return *hits[0];
    if (hits.size() > 1) return std::nullopt;  // ambiguous at the longest suffix: no guess
  }
  return std::nullopt;
}

bool GitRepository::has_commit(const std::string& sha) const {
  if (!ref_is_safe(sha)) return false;
  return git(root_, {"cat-file", "-e", sha + "^{commit}"}).ok;
}

}  // namespace mpi::code
