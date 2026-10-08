#include "adapters/gitlab/ci_log.hpp"

#include <algorithm>
#include <string_view>

namespace mpi::intelligence {
namespace {

using sv = std::string_view;

constexpr std::size_t kMaxTextBytes = 300;
constexpr std::size_t kMaxCandidates = 20;
// A Python traceback longer than this is not looked through for its closing
// exception line: past it, the "exception" found would be a guess.
constexpr std::size_t kTracebackWindow = 500;

bool starts_with(sv s, sv p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(sv s, sv p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

sv trim(sv s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

bool has_text(sv s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return !is_space(c); });
}

// At most `max` bytes of `s`, never splitting a UTF-8 sequence.
sv cut_utf8(sv s, std::size_t max) {
  if (s.size() <= max) return s;
  std::size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) n--;
  return s.substr(0, n);
}

// Candidate and root text: trimmed, and marked when it had to be cut so a
// reader never takes a prefix for the whole message.
std::string short_text(sv t) {
  if (t.size() <= kMaxTextBytes) return std::string(t);
  std::string out(cut_utf8(t, kMaxTextBytes - 3));
  out += "...";
  return out;
}

// ---- cleaning ----

// The index just past the escape sequence that starts at s[i] (an ESC).
std::size_t skip_escape(sv s, std::size_t i) {
  const std::size_t n = s.size();
  std::size_t j = i + 1;
  if (j >= n) return n;
  const auto c = static_cast<unsigned char>(s[j]);
  if (c == '[') {
    // CSI: parameter and intermediate bytes (0x20-0x3F), then one final byte.
    j++;
    while (j < n && static_cast<unsigned char>(s[j]) >= 0x20 &&
           static_cast<unsigned char>(s[j]) <= 0x3F) {
      j++;
    }
    if (j < n && static_cast<unsigned char>(s[j]) >= 0x40 &&
        static_cast<unsigned char>(s[j]) <= 0x7E) {
      j++;
    }
    return j;
  }
  if (c == ']') {
    // OSC (hyperlinks, titles): up to BEL or ESC '\'. Unterminated, it ends
    // with the line -- the caller only ever hands one line in.
    for (j++; j < n; j++) {
      if (s[j] == '\x07') return j + 1;
      if (s[j] == '\x1b' && j + 1 < n && s[j + 1] == '\\') return j + 2;
    }
    return n;
  }
  if (c >= 0x20 && c <= 0x2F) return std::min(n, j + 2);  // ESC ( B and friends
  return j + 1;                                             // two-byte forms
}

void append_without_ansi(sv in, std::string& out) {
  std::size_t i = 0;
  while (i < in.size()) {
    const std::size_t esc = in.find('\x1b', i);
    if (esc == sv::npos) {
      out.append(in.substr(i));
      return;
    }
    out.append(in.substr(i, esc - i));
    i = skip_escape(in, esc);
  }
}

bool is_section_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}

// The length of a GitLab section marker starting at s[i], or 0 when what is
// there only looks like one. Exact grammar rather than "up to the next \r",
// so ordinary text that mentions "section_start:" survives.
std::size_t marker_length(sv s, std::size_t i) {
  const sv rest = s.substr(i);
  std::size_t j = 0;
  if (starts_with(rest, "section_start:")) {
    j = 14;
  } else if (starts_with(rest, "section_end:")) {
    j = 12;
  } else {
    return 0;
  }
  const std::size_t digits = j;
  while (j < rest.size() && rest[j] >= '0' && rest[j] <= '9') j++;
  if (j == digits || j >= rest.size() || rest[j] != ':') return 0;
  j++;
  const std::size_t name = j;
  while (j < rest.size() && is_section_name_char(rest[j])) j++;
  if (j == name) return 0;
  if (j < rest.size() && rest[j] == '[') {  // [collapsed=true]
    const std::size_t close = rest.find(']', j);
    if (close != sv::npos) j = close + 1;
  }
  if (j < rest.size() && rest[j] == '\r') j++;
  return j;
}

// Appends the cleaned form of one line (no '\n') to `out`. False when the
// line held only section markers and is dropped.
bool clean_line(sv line, std::string& out, std::string& work) {
  // Most lines of most logs need nothing; keep them a single copy.
  if (line.find_first_of("\x1b\r") == sv::npos && line.find("section_") == sv::npos) {
    out.append(line);
    return true;
  }
  work.clear();
  bool had_marker = false;
  std::size_t i = 0;
  while (i < line.size()) {
    const std::size_t p = line.find("section_", i);
    if (p == sv::npos) {
      work.append(line.substr(i));
      break;
    }
    const std::size_t len = marker_length(line, p);
    if (len == 0) {
      work.append(line.substr(i, p + 8 - i));
      i = p + 8;
      continue;
    }
    work.append(line.substr(i, p - i));
    had_marker = true;
    i = p + len;
  }
  // A carriage return rewrote the line: what stayed on screen is the last
  // segment that still has text once its escapes are gone.
  const sv w(work);
  const std::size_t mark = out.size();
  std::size_t seg_end = w.size();
  for (;;) {
    const std::size_t cr = seg_end == 0 ? sv::npos : w.rfind('\r', seg_end - 1);
    const std::size_t seg_start = cr == sv::npos ? 0 : cr + 1;
    out.resize(mark);
    append_without_ansi(w.substr(seg_start, seg_end - seg_start), out);
    if (cr == sv::npos || has_text(sv(out).substr(mark))) break;
    seg_end = cr;
  }
  if (had_marker && !has_text(sv(out).substr(mark))) {
    out.resize(mark);
    return false;
  }
  return true;
}

std::vector<sv> split_lines(const std::string& cleaned) {
  std::vector<sv> lines;
  std::size_t pos = 0;
  while (pos < cleaned.size()) {
    const std::size_t nl = cleaned.find('\n', pos);
    const std::size_t end = nl == std::string::npos ? cleaned.size() : nl;
    lines.emplace_back(cleaned.data() + pos, end - pos);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return lines;
}

// ---- classification ----

struct Match {
  const char* rule = nullptr;
  int priority = 0;
};

bool is_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '$' || c == '.';
}

bool is_compiler_error(sv t) {
  if (starts_with(t, "xcodebuild:")) return false;  // its own rule
  if (starts_with(t, "e: ") || starts_with(t, "error[E") || starts_with(t, "error TS")) return true;
  // `file:12:5: error:` / `File.java:12: error:` / `a.ts(3,7): error TS2322:`
  for (std::size_t p = t.find(": error"); p != sv::npos; p = t.find(": error", p + 1)) {
    const sv after = t.substr(p + 7);
    if (starts_with(after, ":") || starts_with(after, " TS")) return true;
  }
  return t.find(": fatal error:") != sv::npos;
}

bool is_test_failure(sv t) {
  if (starts_with(t, "\xE2\x9C\x95 ") || starts_with(t, "\xE2\x9C\x97 ")) return true;  // ✕ ✗
  if (starts_with(t, "FAIL ") || starts_with(t, "FAILED ") || starts_with(t, "--- FAIL:")) return true;
  if (starts_with(t, "Test Case '") && t.find("' failed (") != sv::npos) return true;
  // Gradle: `com.acme.FooTest > addsNumbers FAILED`
  if (ends_with(t, " FAILED") && !starts_with(t, "> ")) {
    const std::size_t gt = t.find(" > ");
    return gt != sv::npos && gt > 0;
  }
  return false;
}

// `TypeError: x`, `java.lang.IllegalStateException: x`, `ValueError: x`.
bool is_named_error(sv t) {
  if (starts_with(t, "Uncaught ")) t.remove_prefix(9);
  std::size_t k = 0;
  while (k < t.size() && is_ident_char(t[k])) k++;
  if (k == t.size() || t[k] != ':') return false;
  const sv name = t.substr(0, k);
  return ends_with(name, "Error") || ends_with(name, "Exception");
}

Match npm_match(sv t) {
  sv rest;
  if (starts_with(t, "npm ERR!")) {
    rest = t.substr(8);
  } else if (t == "npm error" || starts_with(t, "npm error ")) {
    rest = t.substr(9);
  } else {
    return {};
  }
  rest = trim(rest);
  // npm's bookkeeping around the real message.
  for (const sv meta : {sv("code "), sv("errno "), sv("path "), sv("syscall "), sv("command "),
                        sv("cwd "), sv("signal "), sv("A complete log"), sv("Log files were not")}) {
    if (starts_with(rest, meta)) return {"npm_error_meta", 30};
  }
  if (rest.empty()) return {"npm_error_meta", 30};
  return {"npm_error", 65};
}

// The highest-priority rule `t` (a trimmed, non-empty line) matches on its
// own; the two rules that depend on earlier lines are applied by the caller.
Match classify(sv t) {
  if (is_compiler_error(t)) return {"compiler_error", 100};
  if (is_test_failure(t)) return {"test_failure", 95};
  if (starts_with(t, "Caused by: ")) return {"jvm_caused_by", 86};
  if (starts_with(t, "Exception in thread ")) return {"jvm_exception", 85};
  if (starts_with(t, "xcodebuild: error")) return {"xcodebuild_error", 80};
  if (starts_with(t, "error: ")) return {"error_line", 80};
  {
    sv u = t;
    if (starts_with(u, "> ")) u.remove_prefix(2);
    if (starts_with(u, "Execution failed for task")) return {"gradle_execution_failed", 75};
  }
  if (is_named_error(t)) return {"js_error", 70};
  if (const Match m = npm_match(t); m.rule != nullptr) return m;
  if (starts_with(t, "> Task ") && ends_with(t, " FAILED")) return {"gradle_task_failed", 60};
  if (starts_with(t, "ERROR: Job failed")) return {"runner_job_failed", 10};
  if (starts_with(t, "ERROR: ")) return {"error_prefix", 55};
  if (starts_with(t, "fatal: ")) return {"fatal", 50};
  if (starts_with(t, "** ") && t.find(" FAILED **") != sv::npos) return {"xcodebuild_failed", 45};
  if (starts_with(t, "The following build commands failed:") ||
      starts_with(t, "error Command failed with exit code")) {
    return {"build_summary", 40};
  }
  if (starts_with(t, "FAILURE: Build failed with an exception") || starts_with(t, "BUILD FAILED in ")) {
    return {"gradle_build_failed", 35};
  }
  return {};
}

// Keeps the kMaxCandidates most significant candidates seen so far. Lines
// arrive in log order, so a newcomer of equal priority is never better than
// one already kept: the earliest line of a priority always survives, and with
// it the root.
void keep_candidate(std::vector<FailureCandidate>& kept, FailureCandidate c) {
  if (kept.size() < kMaxCandidates) {
    kept.push_back(std::move(c));
    return;
  }
  auto worst = kept.begin();
  for (auto it = kept.begin(); it != kept.end(); ++it) {
    if (it->priority < worst->priority || (it->priority == worst->priority && it->line > worst->line)) {
      worst = it;
    }
  }
  if (c.priority > worst->priority) *worst = std::move(c);
}

// One excerpt line: long lines (minified bundles, base64 blobs) are cut so a
// single line cannot crowd the context out of the excerpt.
std::string excerpt_piece(sv line, std::size_t cap) {
  if (line.size() <= cap) return std::string(line);
  std::string out(cut_utf8(line, cap));
  out += " [... " + std::to_string(line.size() - out.size()) + " bytes cut]";
  return out;
}

}  // namespace

json::Value FailureSummary::to_json() const {
  auto n = [](std::size_t v) { return json::Value::integer(static_cast<std::int64_t>(v)); };
  json::Value o = json::Value::object();
  o.set("found", json::Value::boolean(found));
  if (found) {
    o.set("root_error", json::Value::string(root_error));
    o.set("root_rule", json::Value::string(root_rule));
    o.set("root_line", n(root_line));
    o.set("excerpt_start", n(excerpt_start));
    o.set("excerpt_end", n(excerpt_end));
    o.set("excerpt", json::Value::string(excerpt));
  }
  json::Value cs = json::Value::array();
  for (const auto& c : candidates) {
    json::Value one = json::Value::object();
    one.set("line", n(c.line));
    one.set("text", json::Value::string(c.text));
    one.set("rule", json::Value::string(c.rule));
    one.set("priority", json::Value::integer(c.priority));
    cs.push_back(std::move(one));
  }
  o.set("candidates", std::move(cs));
  o.set("total_lines", n(total_lines));
  return o;
}

std::string clean_ci_log(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  std::string work;
  const sv all(raw);
  std::size_t pos = 0;
  while (pos < all.size()) {
    const std::size_t nl = all.find('\n', pos);
    const std::size_t end = nl == sv::npos ? all.size() : nl;
    if (clean_line(all.substr(pos, end - pos), out, work)) out.push_back('\n');
    if (nl == sv::npos) break;
    pos = nl + 1;
  }
  return out;
}

FailureSummary find_failure(const std::string& raw_log, std::size_t context_before,
                            std::size_t context_after, std::size_t max_excerpt_bytes) {
  FailureSummary s;
  const std::string cleaned = clean_ci_log(raw_log);
  const std::vector<sv> lines = split_lines(cleaned);
  s.total_lines = lines.size();

  std::vector<FailureCandidate> kept;
  bool after_what_went_wrong = false;
  std::size_t traceback_at = 0;  // line of an open `Traceback`, 0 = none
  for (std::size_t i = 0; i < lines.size(); i++) {
    const std::size_t ln = i + 1;
    const sv t = trim(lines[i]);
    if (t.empty()) continue;
    Match m = classify(t);
    if (after_what_went_wrong) {
      after_what_went_wrong = false;
      if (m.priority < 90) m = {"gradle_what_went_wrong", 90};
    }
    if (traceback_at != 0) {
      if (ln - traceback_at > kTracebackWindow) {
        traceback_at = 0;
      } else if (!is_space(lines[i].front()) && !starts_with(t, "Traceback ")) {
        traceback_at = 0;
        if (m.priority < 85) m = {"python_traceback", 85};
      }
    }
    if (t == "* What went wrong:") after_what_went_wrong = true;
    if (starts_with(t, "Traceback (most recent call last)")) traceback_at = ln;
    if (m.rule == nullptr) continue;
    keep_candidate(kept, FailureCandidate{ln, short_text(t), m.rule, m.priority});
  }
  std::sort(kept.begin(), kept.end(),
            [](const FailureCandidate& a, const FailureCandidate& b) { return a.line < b.line; });
  s.candidates = std::move(kept);
  const FailureCandidate* root = nullptr;
  for (const auto& c : s.candidates) {
    if (root == nullptr || c.priority > root->priority) root = &c;  // log order: earliest wins ties
  }
  if (root == nullptr) return s;
  s.found = true;
  s.root_error = root->text;
  s.root_rule = root->rule;
  s.root_line = root->line;

  // The context window, then shrunk from its far ends until it fits.
  std::size_t first = s.root_line > context_before ? s.root_line - context_before : 1;
  std::size_t last = std::min(s.total_lines, s.root_line + context_after);
  const std::size_t cap = std::max<std::size_t>(256, max_excerpt_bytes / 8);
  std::vector<std::string> pieces;
  pieces.reserve(last - first + 1);
  std::size_t total = 0;
  for (std::size_t ln = first; ln <= last; ln++) {
    pieces.push_back(excerpt_piece(lines[ln - 1], cap));
    total += pieces.back().size() + 1;
  }
  total -= 1;  // no newline after the last line
  std::size_t lo = 0, hi = pieces.size() - 1;  // indexes into pieces
  const std::size_t root_idx = s.root_line - first;
  while (total > max_excerpt_bytes && (lo < root_idx || hi > root_idx)) {
    if (root_idx - lo > hi - root_idx) {
      total -= pieces[lo].size() + 1;
      lo++;
    } else {
      total -= pieces[hi].size() + 1;
      hi--;
    }
  }
  s.excerpt_start = first + lo;
  s.excerpt_end = first + hi;
  for (std::size_t k = lo; k <= hi; k++) {
    if (k != lo) s.excerpt += '\n';
    s.excerpt += pieces[k];
  }
  if (s.excerpt.size() > max_excerpt_bytes) s.excerpt = std::string(cut_utf8(s.excerpt, max_excerpt_bytes));
  return s;
}

std::string excerpt_lines(const std::string& raw_log, std::size_t first, std::size_t last,
                          std::size_t max_bytes) {
  const std::string cleaned = clean_ci_log(raw_log);
  const std::vector<sv> lines = split_lines(cleaned);
  if (first == 0) first = 1;
  last = std::min(last, lines.size());
  std::string out;
  for (std::size_t ln = first; ln <= last && out.size() <= max_bytes; ln++) {
    if (ln != first) out += '\n';
    out.append(lines[ln - 1]);
  }
  if (out.size() > max_bytes) out = std::string(cut_utf8(out, max_bytes));
  return out;
}

}  // namespace mpi::intelligence
