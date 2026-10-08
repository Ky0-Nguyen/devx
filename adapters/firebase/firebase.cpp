#include "adapters/firebase/firebase.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>

// Schemas (verified against the official pages on 2026-10-08, see
// fixtures/intelligence/firebase/README.md):
//   Crashlytics: firebase.google.com/docs/crashlytics/bigquery-dataset-schema
//   Performance: firebase.google.com/docs/perf-mon/bigquery-export
//
// What this reader takes from them, and why:
// - Crashlytics has no issue title column; the title shown in the Console is
//   the blamed exception's (Android `exceptions`), error's (Apple non-fatal
//   `error`) or thread's (Apple `threads`) title and subtitle. A row that does
//   carry `issue_title` / `issue_subtitle` (a view someone built) wins.
// - Each Crashlytics event has an `event_id`; the batch table and its
//   `_REALTIME` twin hold the same events, so events are counted by distinct
//   id, never by row -- exporting both must not double a count.
// - Performance tables are one per app and carry no app identifier column,
//   so a metric's bundle id is set only when a row brings `bundle_identifier`.
// - A network request has no `trace_info`; its duration is
//   `network_info.response_completed_time_us`, microseconds after
//   `event_timestamp`, i.e. from request start to the last response byte.

namespace mpi::intelligence {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr std::uint64_t kMaxFileBytes = 256ull * 1024ull * 1024ull;
// The store keeps raw evidence up to 64 MB; the context may ask for less.
constexpr std::uint64_t kMaxRawFileBytes = 64ull * 1024ull * 1024ull;
// Signals are capped by ConnectorContext::max_records, but one signal can
// aggregate millions of rows. This bounds the reading itself.
constexpr std::int64_t kMaxRowsRead = 5'000'000;
constexpr std::size_t kMaxRowBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxFrames = 30;
constexpr std::size_t kMaxOsVersions = 10;
constexpr std::size_t kMaxMessageBytes = 500;
constexpr int kDiscoverPeekRows = 50;
constexpr std::size_t kMaxNotedFailures = 5;

// ---- small JSON helpers ----
// BigQuery exports INT64 as JSON strings ("line": "42") and may export a BOOL
// as "true", so every reader accepts both spellings.

std::string text(const Value* v) {
  if (v == nullptr) return {};
  if (v->is_string()) return v->as_string();
  if (v->is_int()) return std::to_string(v->as_int());
  return {};
}

std::string text(const Value& o, const char* key) { return text(o.find(key)); }

std::optional<std::string> opt_text(const Value& o, const char* key) {
  std::string s = text(o, key);
  if (s.empty()) return std::nullopt;
  return s;
}

const Value* dig(const Value& o, std::initializer_list<const char*> path) {
  const Value* cur = &o;
  for (const char* k : path) {
    if (cur == nullptr || !cur->is_object()) return nullptr;
    cur = cur->find(k);
  }
  return cur;
}

std::optional<std::int64_t> integer(const Value* v) {
  if (v == nullptr) return std::nullopt;
  if (v->is_int()) return v->as_int();
  if (v->is_double()) {
    const double d = v->as_double();
    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9e18) {
      return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
  }
  if (v->is_string() && !v->as_string().empty()) {
    errno = 0;
    char* end = nullptr;
    const long long x = std::strtoll(v->as_string().c_str(), &end, 10);
    if (errno == 0 && end != nullptr && *end == '\0') return static_cast<std::int64_t>(x);
  }
  return std::nullopt;
}

std::optional<double> real(const Value* v) {
  if (v == nullptr) return std::nullopt;
  if (v->is_number()) {
    const double d = v->as_double();
    if (std::isfinite(d)) return d;
    return std::nullopt;
  }
  if (v->is_string() && !v->as_string().empty()) {
    const std::string& s = v->as_string();
    const char c0 = s[0];
    if (!(std::isdigit(static_cast<unsigned char>(c0)) || c0 == '-' || c0 == '+' || c0 == '.')) {
      return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (errno == 0 && end != nullptr && *end == '\0' && std::isfinite(d)) return d;
  }
  return std::nullopt;
}

std::optional<bool> flag(const Value* v) {
  if (v == nullptr) return std::nullopt;
  if (v->is_bool()) return v->as_bool();
  if (v->is_string()) {
    if (v->as_string() == "true") return true;
    if (v->as_string() == "false") return false;
  }
  return std::nullopt;
}

// Whole numbers stay integers; anything else keeps microsecond precision,
// which is all a duration measured in microseconds has.
Value num(double x) {
  if (std::isfinite(x) && std::floor(x) == x && std::fabs(x) < 9e15) {
    return Value::integer(static_cast<std::int64_t>(x));
  }
  return Value::number(std::round(x * 1000.0) / 1000.0);
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex16(std::uint64_t h) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// make_signal_id cuts ids at 64 characters, and a Crashlytics issue id alone
// is 32. Two versions of one issue would then share a file and overwrite each
// other. When the id would be cut, the tail is replaced by a hash of the
// whole external id, so it stays stable and distinct.
std::string stable_signal_id(const std::string& connector_id, const std::string& external_id) {
  std::string id = signals::make_signal_id(connector_id, external_id);
  if (id.size() < 64) return id;
  return id.substr(0, 64 - 17) + "_" + hex16(fnv1a(connector_id + "\n" + external_id));
}

std::string iso_from_us(std::int64_t us) {
  // Floor, so a pre-1970 fraction does not round toward the epoch.
  std::int64_t s = us / 1'000'000;
  if (us % 1'000'000 < 0) s -= 1;
  return signals::format_iso8601(s);
}

// ---- timestamps ----

bool digits_at(const std::string& s, std::size_t from, std::size_t n) {
  if (from + n > s.size()) return false;
  for (std::size_t i = from; i < from + n; i++) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

std::optional<std::int64_t> epoch_int_to_us(std::int64_t t) {
  const std::int64_t a = t < 0 ? -t : t;
  if (a >= 100'000'000'000'000'000) return t / 1000;  // nanoseconds
  if (a >= 100'000'000'000'000) return t;             // microseconds
  if (a >= 100'000'000'000) return t * 1000;          // milliseconds
  return t * 1'000'000;                               // seconds
}

std::optional<std::int64_t> epoch_real_to_us(double t) {
  if (!std::isfinite(t)) return std::nullopt;
  const double a = std::fabs(t);
  if (a >= 9e18) return std::nullopt;
  if (a >= 1e17) return std::llround(t / 1000.0);
  if (a >= 1e14) return std::llround(t);
  if (a >= 1e11) return std::llround(t * 1e3);
  return std::llround(t * 1e6);
}

std::optional<std::int64_t> parse_text_time_us(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t lead = 0;
  while (lead < s.size() && std::isspace(static_cast<unsigned char>(s[lead]))) lead++;
  s.erase(0, lead);
  // BigQuery writes "2026-10-08 01:15:44.123456 UTC".
  if (ends_with(s, " UTC")) {
    s.resize(s.size() - 4);
  } else if (ends_with(s, "UTC")) {
    s.resize(s.size() - 3);
  }
  if (!(digits_at(s, 0, 4) && s.size() >= 10 && s[4] == '-' && digits_at(s, 5, 2) &&
        s[7] == '-' && digits_at(s, 8, 2))) {
    return std::nullopt;
  }
  std::int64_t frac_us = 0;
  std::size_t i = 10;
  if (i < s.size()) {
    if (s[i] != 'T' && s[i] != ' ') return std::nullopt;
    if (!(digits_at(s, i + 1, 2) && s.size() > i + 3 && s[i + 3] == ':' && digits_at(s, i + 4, 2))) {
      return std::nullopt;
    }
    i += 6;
    if (i < s.size() && s[i] == ':') {
      if (!digits_at(s, i + 1, 2)) return std::nullopt;
      i += 3;
    }
    if (i < s.size() && s[i] == '.') {
      const std::size_t start = ++i;
      std::int64_t scale = 100000;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (scale > 0) {
          frac_us += (s[i] - '0') * scale;
          scale /= 10;
        }
        i++;
      }
      if (i == start) return std::nullopt;
    }
    // Only UTC or a numeric offset. A zone name ("PST") is refused rather
    // than silently read as UTC.
    const std::string zone = s.substr(i);
    if (zone == "Z" || zone.empty()) {
      s.resize(i);
    } else if ((zone[0] == '+' || zone[0] == '-') &&
               ((zone.size() == 3 && digits_at(zone, 1, 2)) ||
                (zone.size() == 5 && digits_at(zone, 1, 4)) ||
                (zone.size() == 6 && digits_at(zone, 1, 2) && zone[3] == ':' &&
                 digits_at(zone, 4, 2)))) {
      // parse_iso8601 reads "+hh:mm"; spell "+hhmm" that way first.
      if (zone.size() == 5) s = s.substr(0, i) + zone.substr(0, 3) + ":" + zone.substr(3);
    } else {
      return std::nullopt;
    }
  }
  const auto secs = signals::parse_iso8601(s);
  if (!secs) return std::nullopt;
  return *secs * 1'000'000 + frac_us;
}

// ---- reading export files ----

struct ExportFile {
  std::string name, path;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
};

std::string expand_home(const std::string& p) {
  if (p == "~" || p.rfind("~/", 0) == 0) {
    if (const char* home = std::getenv("HOME")) return std::string(home) + p.substr(1);
  }
  return p;
}

bool stat_file(const std::string& path, std::uint64_t* size, std::int64_t* mtime, bool* regular) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  *regular = S_ISREG(st.st_mode);
  *size = static_cast<std::uint64_t>(st.st_size);
  *mtime = static_cast<std::int64_t>(st.st_mtime);
  return true;
}

// Export files in `dir`, sorted by name so every sync reads them in the same
// order and every "first" choice below is deterministic.
std::vector<ExportFile> list_exports(const std::string& dir, std::vector<std::string>* notes) {
  std::vector<ExportFile> out;
  std::error_code ec;
  int skipped_compressed = 0;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::string lname = name;
    std::transform(lname.begin(), lname.end(), lname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ends_with(lname, ".gz") || ends_with(lname, ".avro") || ends_with(lname, ".parquet")) {
      skipped_compressed++;
      continue;
    }
    if (!(ends_with(lname, ".json") || ends_with(lname, ".jsonl") || ends_with(lname, ".ndjson"))) {
      continue;
    }
    ExportFile f;
    f.name = name;
    f.path = it->path().string();
    bool regular = false;
    if (!stat_file(f.path, &f.size, &f.mtime, &regular) || !regular) continue;
    out.push_back(std::move(f));
  }
  if (skipped_compressed > 0 && notes != nullptr) {
    notes->push_back(std::to_string(skipped_compressed) +
                     " compressed, Avro or Parquet export file(s) were not read: export with "
                     "--destination_format=NEWLINE_DELIMITED_JSON and no compression");
  }
  std::sort(out.begin(), out.end(),
            [](const ExportFile& a, const ExportFile& b) { return a.name < b.name; });
  return out;
}

// Reads one export file a row at a time, so memory holds one row rather than
// the file. Two shapes: NDJSON (`bq extract`), one object per line, where a
// bad line costs only that line; and a JSON array (`bq query --format=json`),
// whose elements are split by a small scanner that tracks strings and
// nesting, so each element is parsed alone.
class RowReader {
 public:
  explicit RowReader(const std::string& path) : in_(path, std::ios::binary) {
    if (!in_.is_open()) {
      error_ = "cannot open the file";
      done_ = true;
      return;
    }
    buf_ = in_.rdbuf();
    // Leading blank lines still count, so line numbers match an editor's.
    while (is_space(buf_->sgetc())) {
      if (buf_->sbumpc() == '\n') number_++;
    }
    if (buf_->sgetc() == '[') {
      buf_->sbumpc();
      array_ = true;
      number_ = 0;
    }
  }

  bool array_mode() const { return array_; }
  /// 1-based line (NDJSON) or element (array) number of the last row.
  std::int64_t number() const { return number_; }
  /// Non-empty when the file's structure is broken (arrays only).
  const std::string& error() const { return error_; }

  bool next(std::string& row) {
    row.clear();
    if (done_) return false;
    return array_ ? next_element(row) : next_line(row);
  }

 private:
  static constexpr int kEof = std::char_traits<char>::eof();
  static bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  bool next_line(std::string& row) {
    while (true) {
      int c = buf_->sbumpc();
      if (c == kEof) {
        done_ = true;
        return false;
      }
      number_++;
      while (c != kEof && c != '\n') {
        row.push_back(static_cast<char>(c));
        c = buf_->sbumpc();
      }
      if (c == kEof) done_ = true;
      while (!row.empty() && is_space(static_cast<unsigned char>(row.back()))) row.pop_back();
      std::size_t lead = 0;
      while (lead < row.size() && is_space(static_cast<unsigned char>(row[lead]))) lead++;
      if (lead < row.size()) {
        row.erase(0, lead);
        return true;
      }
      row.clear();
      if (done_) return false;
    }
  }

  int next_non_space() {
    int c = buf_->sbumpc();
    while (is_space(c)) c = buf_->sbumpc();
    return c;
  }

  bool fail(const std::string& why) {
    error_ = why + " (after element " + std::to_string(number_) + ")";
    done_ = true;
    return false;
  }

  bool next_element(std::string& row) {
    int c = next_non_space();
    if (c == ']') return finish();
    if (number_ > 0) {
      if (c != ',') return fail(c == kEof ? "the JSON array is not closed" : "expected ',' or ']'");
      c = next_non_space();
    }
    if (c == kEof) return fail("the JSON array is not closed");
    if (c == ',' || c == ']') return fail("an empty array element");
    const char first = static_cast<char>(c);
    int depth = 0;
    bool in_str = false, esc = false;
    while (true) {
      row.push_back(static_cast<char>(c));
      if (row.size() > kMaxRowBytes) return fail("an element is larger than 16 MB");
      if (in_str) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          in_str = false;
        }
      } else if (c == '"') {
        in_str = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        if (--depth < 0) return fail("unbalanced brackets");
      }
      if (!in_str && depth == 0) {
        if (first == '{' || first == '[' || first == '"') break;
        const int nx = buf_->sgetc();
        if (nx == ',' || nx == ']' || is_space(nx) || nx == kEof) break;
      }
      c = buf_->sbumpc();
      if (c == kEof) return fail("the JSON array is not closed");
    }
    number_++;
    return true;
  }

  bool finish() {
    if (next_non_space() != kEof) return fail("text after the closing ']'");
    done_ = true;
    return false;
  }

  std::ifstream in_;
  std::streambuf* buf_ = nullptr;
  bool array_ = false;
  bool done_ = false;
  std::int64_t number_ = 0;
  std::string error_;
};

// A JSON array is checked end to end before any of it is used: a truncated
// array would otherwise contribute half its events, and a half-counted issue
// looks exactly like a fixed one.
std::string array_structure_error(const std::string& path) {
  RowReader r(path);
  if (!r.array_mode()) return r.error();
  std::string row;
  while (r.next(row)) {
  }
  return r.error();
}

std::optional<std::string> read_whole(const std::string& path, std::uint64_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string s(static_cast<std::size_t>(size), '\0');
  in.read(s.data(), static_cast<std::streamsize>(size));
  if (static_cast<std::uint64_t>(in.gcount()) != size) return std::nullopt;
  return s;
}

// ---- settings ----

struct Settings {
  std::string export_dir;
  std::string app;
  std::string kind = "auto";
  std::string error;
};

Settings read_settings(const signals::ConnectorConfig& c) {
  Settings s;
  s.export_dir = expand_home(text(c.settings, "export_dir"));
  s.app = text(c.settings, "app_identifier");
  const std::string kind = text(c.settings, "dataset_kind");
  if (!kind.empty()) s.kind = kind;
  if (s.export_dir.empty()) {
    s.error = "export_dir is required: the directory holding your BigQuery export files";
  } else if (s.kind != "auto" && s.kind != "crashlytics" && s.kind != "performance") {
    s.error = "dataset_kind is auto, crashlytics or performance, not '" + s.kind + "'";
  }
  return s;
}

std::string check_dir(const std::string& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return "export_dir " + dir + " does not exist";
  if (!fs::is_directory(dir, ec)) return "export_dir " + dir + " is not a directory";
  std::error_code ec2;
  fs::directory_iterator it(dir, ec2);
  if (ec2) return "export_dir " + dir + " cannot be read: " + ec2.message();
  return {};
}

// ---- rows ----

enum class RowKind { kCrash, kPerf, kOther };

RowKind classify(const Value& row) {
  if (!row.is_object()) return RowKind::kOther;
  if (!text(row, "issue_id").empty() && !text(row, "event_id").empty()) return RowKind::kCrash;
  if (!text(row, "event_type").empty() && !text(row, "event_name").empty()) {
    const Value* t = row.find("trace_info");
    const Value* n = row.find("network_info");
    if ((t != nullptr && t->is_object()) || (n != nullptr && n->is_object())) return RowKind::kPerf;
  }
  return RowKind::kOther;
}

const char* kind_name(RowKind k) {
  switch (k) {
    case RowKind::kCrash: return "crashlytics";
    case RowKind::kPerf: return "performance";
    case RowKind::kOther: return "unknown";
  }
  return "unknown";
}

void add_once(std::vector<std::string>& files, const std::string& name) {
  // Files are read in order, so a repeat is always the last entry.
  if (files.empty() || files.back() != name) files.push_back(name);
}

void widen(std::optional<std::int64_t>& first, std::optional<std::int64_t>& last,
           std::optional<std::int64_t> t) {
  if (!t) return;
  if (!first || *t < *first) first = t;
  if (!last || *t > *last) last = t;
}

// The exception, error or thread Crashlytics blames: the record flagged
// `blamed`, else (for threads) the crashed one, else the first record.
const Value* pick_record(const Value& row, const char* key, bool prefer_crashed) {
  const Value* a = row.find(key);
  if (a == nullptr || !a->is_array()) return nullptr;
  for (const auto& x : a->items()) {
    if (x.is_object() && flag(x.find("blamed")).value_or(false)) return &x;
  }
  if (prefer_crashed) {
    for (const auto& x : a->items()) {
      if (x.is_object() && flag(x.find("crashed")).value_or(false)) return &x;
    }
  }
  for (const auto& x : a->items()) {
    if (x.is_object()) return &x;
  }
  return nullptr;
}

Value frame_json(const Value& f, bool with_blamed) {
  Value o = Value::object();
  for (const char* k : {"file", "symbol", "library"}) {
    if (auto s = opt_text(f, k)) o.set(k, Value::string(*s));
  }
  if (auto line = integer(f.find("line"))) o.set("line", Value::integer(*line));
  if (with_blamed) {
    if (auto b = flag(f.find("blamed"))) o.set("blamed", Value::boolean(*b));
  }
  return o;
}

std::string clip(std::string s, std::size_t n) {
  if (s.size() > n) {
    s.resize(n);
    // Never leave half a UTF-8 sequence behind.
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80) != 0) s.pop_back();
    s += "...";
  }
  return s;
}

struct CrashGroup {
  std::string issue_id;
  std::optional<std::string> version, build, bundle, platform, title;
  std::set<std::string> event_ids, installs;
  std::map<std::string, std::int64_t> error_types;
  std::optional<std::int64_t> first_us, last_us;
  std::optional<Value> blame_frame, frames, exception;
  std::map<std::string, std::int64_t> os_versions;
  std::vector<std::string> files;
};

struct PerfGroup {
  std::string event_type, event_name;
  std::optional<std::string> method, parent, version, build, bundle;
  std::int64_t rows = 0;
  std::vector<double> values;  // ms, or metric values for TRACE_METRIC
  std::vector<double> slow, frozen, req_bytes, resp_bytes;
  std::map<std::int64_t, std::int64_t> codes;
  std::int64_t without_code = 0;
  std::optional<std::int64_t> first_us, last_us;
  std::vector<std::string> files;
};

std::string key_part(const std::optional<std::string>& s) { return s ? "=" + *s : "!"; }

struct Stats {
  std::int64_t rows = 0, other = 0, filtered_app = 0, filtered_kind = 0, duplicates = 0;
  std::int64_t perf_unknown_app = 0, negative = 0, untimed = 0;
};

class Aggregator {
 public:
  explicit Aggregator(const Settings& s) : s_(s) {}

  void add(const Value& row, const std::string& file) {
    stats.rows++;
    const RowKind k = classify(row);
    if (k == RowKind::kOther) {
      stats.other++;
      return;
    }
    if ((k == RowKind::kCrash && s_.kind == "performance") ||
        (k == RowKind::kPerf && s_.kind == "crashlytics")) {
      stats.filtered_kind++;
      return;
    }
    const auto bundle = opt_text(row, "bundle_identifier");
    if (!s_.app.empty()) {
      if (bundle && *bundle != s_.app) {
        stats.filtered_app++;
        return;
      }
      // Crashlytics rows always name their app, so one that does not cannot
      // be shown to be this app. Performance tables are one per app and
      // never name it, so their rows are kept and counted instead.
      if (!bundle) {
        if (k == RowKind::kCrash) {
          stats.filtered_app++;
          return;
        }
        stats.perf_unknown_app++;
      }
    }
    std::optional<std::int64_t> t;
    if (const Value* ts = row.find("event_timestamp")) t = firebase::parse_timestamp_us(*ts);
    if (!t) stats.untimed++;
    if (t && (!max_us || *t > *max_us)) max_us = t;
    if (k == RowKind::kCrash) {
      add_crash(row, file, bundle, t);
    } else {
      add_perf(row, file, bundle, t);
    }
  }

  std::map<std::string, CrashGroup> crashes;
  std::map<std::string, PerfGroup> perf;
  Stats stats;
  std::optional<std::int64_t> max_us;

 private:
  void add_crash(const Value& row, const std::string& file,
                 const std::optional<std::string>& bundle, std::optional<std::int64_t> t) {
    const std::string issue = text(row, "issue_id");
    std::optional<std::string> ver, build;
    if (const Value* app = row.find("application"); app != nullptr && app->is_object()) {
      ver = opt_text(*app, "display_version");
      build = opt_text(*app, "build_version");
    }
    const std::string key = issue + "\x1f" + key_part(ver) + "\x1f" + key_part(build);
    CrashGroup& g = crashes[key];
    const std::string event_id = text(row, "event_id");
    if (!g.event_ids.insert(event_id).second) {
      // The same event twice: a batch table and its _REALTIME twin, or one
      // export saved twice. Counted once.
      stats.duplicates++;
      return;
    }
    if (g.issue_id.empty()) {
      g.issue_id = issue;
      g.version = ver;
      g.build = build;
    }
    if (!g.bundle) g.bundle = bundle;
    if (!g.platform) g.platform = opt_text(row, "platform");
    add_once(g.files, file);
    widen(g.first_us, g.last_us, t);

    std::string type = upper(text(row, "error_type"));
    if (type.empty()) {
      // `is_fatal` is the deprecated spelling of the same fact.
      if (auto fatal = flag(row.find("is_fatal"))) type = *fatal ? "FATAL" : "NON_FATAL";
    }
    g.error_types[type]++;
    if (auto install = opt_text(row, "installation_uuid")) g.installs.insert(*install);
    if (const Value* os = row.find("operating_system"); os != nullptr && os->is_object()) {
      if (auto v = opt_text(*os, "display_version")) g.os_versions[*v]++;
    }

    // Representative details come from the first row that has them; files
    // and rows are read in a fixed order, so the choice is stable.
    const Value* rec = pick_record(row, "exceptions", false);
    const bool android = rec != nullptr;
    const bool apple_thread = rec == nullptr && pick_record(row, "error", false) == nullptr;
    if (rec == nullptr) rec = pick_record(row, "error", false);
    if (rec == nullptr) rec = pick_record(row, "threads", true);
    if (!g.title) {
      if (auto it = opt_text(row, "issue_title")) {
        std::string title = *it;
        if (auto sub = opt_text(row, "issue_subtitle")) title += " — " + *sub;
        g.title = title;
      } else if (rec != nullptr) {
        if (auto rt = opt_text(*rec, "title")) {
          std::string title = *rt;
          if (auto sub = opt_text(*rec, "subtitle")) title += " — " + *sub;
          g.title = title;
        }
      }
    }
    if (!g.blame_frame) {
      if (const Value* bf = row.find("blame_frame"); bf != nullptr && bf->is_object()) {
        Value f = frame_json(*bf, false);
        if (f.size() > 0) g.blame_frame = std::move(f);
      }
    }
    if (!g.frames && rec != nullptr) {
      if (const Value* fr = rec->find("frames"); fr != nullptr && fr->is_array() && !fr->items().empty()) {
        Value a = Value::array();
        for (const auto& f : fr->items()) {
          if (a.size() >= kMaxFrames) break;
          if (f.is_object()) a.push_back(frame_json(f, true));
        }
        g.frames = std::move(a);
      }
    }
    if (!g.exception && rec != nullptr) {
      Value e = Value::object();
      if (android) {
        if (auto ty = opt_text(*rec, "type")) e.set("type", Value::string(*ty));
        if (auto m = opt_text(*rec, "exception_message")) {
          e.set("message", Value::string(clip(*m, kMaxMessageBytes)));
        }
      } else if (apple_thread) {
        if (auto sn = opt_text(*rec, "signal_name")) e.set("type", Value::string(*sn));
        if (auto sc = opt_text(*rec, "signal_code")) e.set("message", Value::string(*sc));
      }
      if (e.size() > 0) g.exception = std::move(e);
    }
  }

  void add_perf(const Value& row, const std::string& file, const std::optional<std::string>& bundle,
                std::optional<std::int64_t> t) {
    PerfGroup probe;
    probe.event_type = upper(text(row, "event_type"));
    probe.event_name = text(row, "event_name");
    probe.version = opt_text(row, "app_display_version");
    probe.build = opt_text(row, "app_build_version");
    probe.bundle = bundle;
    const bool network = probe.event_type == "NETWORK_REQUEST";
    const bool metric = probe.event_type == "TRACE_METRIC";
    if (network) {
      if (const Value* n = row.find("network_info"); n != nullptr && n->is_object()) {
        if (auto m = opt_text(*n, "request_http_method")) probe.method = upper(*m);
      }
    }
    if (metric) probe.parent = opt_text(row, "parent_trace_name");
    const std::string key = probe.event_type + "\x1f" + probe.event_name + "\x1f" +
                            key_part(probe.method) + "\x1f" + key_part(probe.parent) + "\x1f" +
                            key_part(probe.version) + "\x1f" + key_part(probe.build) + "\x1f" +
                            key_part(probe.bundle);
    auto [it, fresh] = perf.try_emplace(key);
    PerfGroup& g = it->second;
    if (fresh) g = std::move(probe);
    g.rows++;
    add_once(g.files, file);
    widen(g.first_us, g.last_us, t);

    std::optional<double> value;
    if (metric) {
      value = real(dig(row, {"trace_info", "metric_info", "metric_value"}));
    } else {
      std::optional<double> us;
      if (network) us = real(dig(row, {"network_info", "response_completed_time_us"}));
      if (!us) us = real(dig(row, {"trace_info", "duration_us"}));
      if (us && *us < 0) {
        stats.negative++;
        us.reset();
      }
      if (us) value = *us / 1000.0;
    }
    if (value) g.values.push_back(*value);

    if (auto r = real(dig(row, {"trace_info", "screen_info", "slow_frame_ratio"}))) g.slow.push_back(*r);
    if (auto r = real(dig(row, {"trace_info", "screen_info", "frozen_frame_ratio"}))) g.frozen.push_back(*r);
    if (network) {
      if (auto code = integer(dig(row, {"network_info", "response_code"}))) {
        g.codes[*code]++;
      } else {
        g.without_code++;
      }
      if (auto b = real(dig(row, {"network_info", "request_payload_bytes"}))) g.req_bytes.push_back(*b);
      if (auto b = real(dig(row, {"network_info", "response_payload_bytes"}))) g.resp_bytes.push_back(*b);
    }
  }

  const Settings& s_;
};

std::string fmt_ms(double ms) {
  char buf[48];
  if (ms >= 1000.0) {
    std::snprintf(buf, sizeof buf, "%.1f s", ms / 1000.0);
  } else if (ms >= 10.0) {
    std::snprintf(buf, sizeof buf, "%.0f ms", ms);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f ms", ms);
  }
  return buf;
}

std::string fmt_value(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

Value string_list(const std::vector<std::string>& xs) {
  Value a = Value::array();
  for (const auto& x : xs) a.push_back(Value::string(x));
  return a;
}

signals::ReleaseIdentity release_of(const std::optional<std::string>& version,
                                    const std::optional<std::string>& build,
                                    const std::optional<std::string>& bundle) {
  signals::ReleaseIdentity r;
  r.version = version;
  r.build_number = build;
  r.bundle_or_package_id = bundle;
  r.source = signals::FactSource::kProvider;
  return r;
}

// Firebase itself states the version and build of every event, so the link
// is provider-attributed -- unless no part of it was stated at all.
signals::EvidenceBasis basis_of(const signals::ReleaseIdentity& r) {
  return r.empty() ? signals::EvidenceBasis::kUnknown : signals::EvidenceBasis::kProviderAttributed;
}

struct Built {
  signals::SignalRecord record;
  std::int64_t weight = 0;  // events or rows, for the order signals are written in
};

Built build_crash(const CrashGroup& g, const std::map<std::string, std::string>& raw_refs) {
  Built b;
  signals::SignalRecord& r = b.record;
  // FATAL and ANR end the session; NON_FATAL does not. Any other type is
  // reported as an issue with no severity rather than guessed at.
  static const char* kPriority[] = {"FATAL", "ANR", "NON_FATAL"};
  std::string type;
  for (const char* p : kPriority) {
    if (g.error_types.count(p) != 0) {
      type = p;
      break;
    }
  }
  if (type.empty()) {
    for (const auto& [t, n] : g.error_types) {
      if (!t.empty()) {
        type = t;
        break;
      }
    }
  }
  if (type == "FATAL" || type == "ANR") {
    r.kind = "crash";
    r.severity = "fatal";
  } else if (type == "NON_FATAL") {
    r.kind = "issue";
    r.severity = "error";
  } else {
    r.kind = "issue";
  }
  r.external_id = "crash:" + g.issue_id + ":" + g.version.value_or("") + "+" + g.build.value_or("");
  r.title = g.title.value_or("Crashlytics issue " + g.issue_id);
  if (g.first_us) r.occurred_at = iso_from_us(*g.first_us);
  r.release = release_of(g.version, g.build, g.bundle);
  r.basis = basis_of(r.release);
  if (!g.files.empty()) {
    if (auto it = raw_refs.find(g.files.front()); it != raw_refs.end()) r.raw_ref = it->second;
  }
  Value a = Value::object();
  a.set("issue_id", Value::string(g.issue_id));
  if (!type.empty()) a.set("error_type", Value::string(type));
  std::size_t named_types = 0;
  for (const auto& [t, n] : g.error_types) named_types += t.empty() ? 0u : 1u;
  if (g.error_types.size() > 1 || named_types == 0) {
    Value types = Value::object();
    for (const auto& [t, n] : g.error_types) types.set(t.empty() ? "unknown" : t, Value::integer(n));
    a.set("error_types", std::move(types));
  }
  const auto events = static_cast<std::int64_t>(g.event_ids.size());
  a.set("events", Value::integer(events));
  // No installation id on any event means the count is unknown, not zero.
  if (!g.installs.empty()) {
    a.set("affected_installations", Value::integer(static_cast<std::int64_t>(g.installs.size())));
  }
  if (g.first_us) a.set("first_event_at", Value::string(iso_from_us(*g.first_us)));
  if (g.last_us) a.set("last_event_at", Value::string(iso_from_us(*g.last_us)));
  if (g.blame_frame) a.set("blame_frame", *g.blame_frame);
  if (g.frames) a.set("frames", *g.frames);
  if (g.exception) a.set("exception", *g.exception);
  if (g.platform) a.set("platform", Value::string(*g.platform));
  if (!g.os_versions.empty()) {
    std::vector<std::pair<std::string, std::int64_t>> os(g.os_versions.begin(), g.os_versions.end());
    std::stable_sort(os.begin(), os.end(),
                     [](const auto& x, const auto& y) { return x.second > y.second; });
    Value list = Value::array();
    for (std::size_t i = 0; i < os.size() && i < kMaxOsVersions; i++) {
      list.push_back(Value::string(os[i].first));
    }
    a.set("os_versions", std::move(list));
    if (os.size() > kMaxOsVersions) a.set("os_versions_truncated", Value::boolean(true));
  }
  a.set("source_files", string_list(g.files));
  {
    // Every export file this signal was built from, so retention keeps all
    // of them while the signal is kept -- not only the first.
    Value refs = Value::array();
    for (const auto& f : g.files) {
      if (auto it = raw_refs.find(f); it != raw_refs.end()) refs.push_back(Value::string(it->second));
    }
    a.set("raw_refs", std::move(refs));
  }
  r.attributes = std::move(a);
  b.weight = events;
  return b;
}

Value percentile_block(std::vector<double> xs, std::initializer_list<int> ps) {
  std::sort(xs.begin(), xs.end());
  Value o = Value::object();
  for (int p : ps) o.set("p" + std::to_string(p), num(firebase::nearest_rank(xs, p)));
  return o;
}

Value median(std::vector<double> xs) {
  std::sort(xs.begin(), xs.end());
  return num(firebase::nearest_rank(xs, 50));
}

Built build_perf(const PerfGroup& g, const std::string& key,
                 const std::map<std::string, std::string>& raw_refs) {
  Built b;
  signals::SignalRecord& r = b.record;
  const bool network = g.event_type == "NETWORK_REQUEST";
  const bool metric = g.event_type == "TRACE_METRIC";
  r.kind = "metric";
  r.external_id = "perf:" + hex16(fnv1a(key));
  std::string label = g.event_name;
  if (network && g.method) label = *g.method + " " + label;
  if (metric && g.parent) label = *g.parent + "/" + label;
  std::vector<double> sorted = g.values;
  std::sort(sorted.begin(), sorted.end());
  if (sorted.empty()) {
    r.title = label + (metric ? " (no values)" : " (no duration samples)");
  } else {
    const double p95 = firebase::nearest_rank(sorted, 95);
    r.title = label + " p95 " + (metric ? fmt_value(p95) : fmt_ms(p95));
  }
  if (g.first_us) r.occurred_at = iso_from_us(*g.first_us);
  r.release = release_of(g.version, g.build, g.bundle);
  r.basis = basis_of(r.release);
  if (!g.files.empty()) {
    if (auto it = raw_refs.find(g.files.front()); it != raw_refs.end()) r.raw_ref = it->second;
  }
  Value a = Value::object();
  a.set("event_type", Value::string(g.event_type));
  a.set("event_name", Value::string(g.event_name));
  if (g.method) a.set("http_method", Value::string(*g.method));
  if (g.parent) a.set("parent_trace_name", Value::string(*g.parent));
  // Metric values carry whatever unit the app's code counted in.
  a.set("unit", Value::string(metric ? "as reported" : "ms"));
  a.set("events", Value::integer(g.rows));
  a.set("samples", Value::integer(static_cast<std::int64_t>(sorted.size())));
  if (!sorted.empty()) {
    a.set("percentile_method", Value::string("nearest_rank"));
    for (int p : {50, 90, 95, 99}) a.set("p" + std::to_string(p), num(firebase::nearest_rank(sorted, p)));
    a.set("min", num(sorted.front()));
    a.set("max", num(sorted.back()));
  }
  if (network) {
    Value codes = Value::object();
    std::int64_t with_code = 0, ok = 0;
    for (const auto& [code, n] : g.codes) {
      codes.set(std::to_string(code), Value::integer(n));
      with_code += n;
      if (code >= 200 && code < 300) ok += n;
    }
    a.set("response_codes", std::move(codes));
    // Share of requests that got a 2xx, among those that got any answer; a
    // request with no response code is not counted as a failure, nor as a
    // success.
    if (with_code > 0) {
      a.set("success_rate", Value::number(static_cast<double>(ok) / static_cast<double>(with_code)));
    }
    if (g.without_code > 0) a.set("requests_without_response_code", Value::integer(g.without_code));
    if (!g.req_bytes.empty()) a.set("request_payload_bytes_p50", median(g.req_bytes));
    if (!g.resp_bytes.empty()) a.set("response_payload_bytes_p50", median(g.resp_bytes));
  }
  if (!g.slow.empty()) a.set("slow_frame_ratio", percentile_block(g.slow, {50, 95}));
  if (!g.frozen.empty()) a.set("frozen_frame_ratio", percentile_block(g.frozen, {50, 95}));
  if (g.first_us) a.set("first_event_at", Value::string(iso_from_us(*g.first_us)));
  if (g.last_us) a.set("last_event_at", Value::string(iso_from_us(*g.last_us)));
  a.set("source_files", string_list(g.files));
  {
    // Every export file this signal was built from, so retention keeps all
    // of them while the signal is kept -- not only the first.
    Value refs = Value::array();
    for (const auto& f : g.files) {
      if (auto it = raw_refs.find(f); it != raw_refs.end()) refs.push_back(Value::string(it->second));
    }
    a.set("raw_refs", std::move(refs));
  }
  r.attributes = std::move(a);
  b.weight = g.rows;
  return b;
}

// ---- the connector ----

class FirebaseConnector : public signals::SignalConnector {
 public:
  signals::ConnectorInfo info() const override {
    signals::ConnectorInfo i;
    i.provider = "firebase";
    i.display_name = "Firebase (BigQuery export)";
    i.category = "production";
    i.egress = "Nothing: reads export files on this Mac";
    i.credential_help = "";
    // Export is switched on in the console (BigQuery card, Link).
    i.setup_url = "https://console.firebase.google.com/project/_/settings/integrations";
    i.settings = {
        {"export_dir",
         "Required. A directory of Crashlytics and/or Performance Monitoring BigQuery exports: "
         "*.json, *.jsonl or *.ndjson, each a JSON array (bq query --format=json) or one row per "
         "line (bq extract --destination_format=NEWLINE_DELIMITED_JSON)."},
        {"app_identifier",
         "Optional. Only rows whose bundle_identifier is this bundle id / package name. "
         "Performance rows name no app (one table per app) and are kept."},
        {"dataset_kind", "auto (default), crashlytics or performance: which rows to read."},
    };
    return i;
  }

  signals::ConnectorCapabilities capabilities() const override {
    signals::ConnectorCapabilities c;
    c.crashes = true;
    c.issues = true;
    c.metrics = true;
    c.incremental_pull = true;
    c.network = false;
    return c;
  }

  signals::ValidateResult validate(const signals::ConnectorConfig& config,
                                   const signals::ConnectorContext&) override {
    signals::ValidateResult r;
    const Settings s = read_settings(config);
    if (!s.error.empty()) {
      r.error = s.error;
      return r;
    }
    r.error = check_dir(s.export_dir);
    if (!r.error.empty()) return r;
    const auto files = list_exports(s.export_dir, &r.notes);
    r.notes.push_back(std::to_string(files.size()) + " export file(s) in " + s.export_dir);
    if (files.empty()) {
      r.notes.push_back("export with: bq extract --destination_format=NEWLINE_DELIMITED_JSON "
                        "<dataset>.<table> gs://<bucket>/export-*.json, then copy the files here");
    }
    r.notes.push_back("reads local files only; nothing leaves this Mac");
    r.ok = true;
    return r;
  }

  signals::DiscoverResult discover(const signals::ConnectorConfig& config,
                                   const signals::ConnectorContext&) override {
    signals::DiscoverResult r;
    const Settings s = read_settings(config);
    r.error = !s.error.empty() ? s.error : check_dir(s.export_dir);
    if (!r.error.empty()) return r;
    for (const auto& f : list_exports(s.export_dir, nullptr)) {
      RowKind kind = RowKind::kOther;
      if (f.size <= kMaxFileBytes) {
        RowReader reader(f.path);
        std::string row;
        for (int i = 0; i < kDiscoverPeekRows && kind == RowKind::kOther && reader.next(row); i++) {
          if (auto v = json::parse(row, nullptr)) kind = classify(*v);
        }
      }
      Value one = Value::object();
      one.set("id", Value::string(f.name));
      one.set("name", Value::string(f.name));
      one.set("kind", Value::string(kind_name(kind)));
      one.set("size", Value::integer(static_cast<std::int64_t>(f.size)));
      r.resources.push_back(std::move(one));
    }
    r.ok = true;
    return r;
  }

  signals::SyncResult sync(const signals::ConnectorConfig& config, const signals::SyncCursor& cursor,
                           signals::SignalSink& sink, const signals::ConnectorContext& ctx) override {
    signals::SyncResult r;
    r.started_at = ctx.now_iso;
    r.cursor = cursor;
    r.cursor.connector_id = config.id;
    r.cursor.resume.clear();
    const Settings s = read_settings(config);
    r.error = !s.error.empty() ? s.error : check_dir(s.export_dir);
    if (!r.error.empty()) {
      r.status = signals::SyncStatus::kFailed;
      return r;
    }

    const auto files = list_exports(s.export_dir, &r.notes);
    if (files.empty()) {
      r.notes.push_back("no *.json, *.jsonl or *.ndjson export files in " + s.export_dir + " yet");
      r.status = signals::SyncStatus::kComplete;
      return r;
    }

    // Every file is read again on every sync: aggregates are recomputed from
    // all rows, so ids stay stable and a count is never added to twice.
    bool partial = false;
    bool capped = false;
    Aggregator agg(s);
    std::map<std::string, std::string> raw_refs;
    Value seen = Value::object();
    const std::uint64_t raw_limit =
        std::min<std::uint64_t>(kMaxRawFileBytes, static_cast<std::uint64_t>(ctx.max_raw_bytes));
    for (const auto& f : files) {
      if (ctx.cancel.cancelled()) {
        // No signal has been written yet: stop clean rather than store
        // aggregates of half the rows.
        r.status = signals::SyncStatus::kFailed;
        r.error = "cancelled before any signal was written";
        return r;
      }
      Value meta = Value::object();
      meta.set("size", Value::integer(static_cast<std::int64_t>(f.size)));
      meta.set("mtime", Value::string(signals::format_iso8601(f.mtime)));
      seen.set(f.name, std::move(meta));
      if (capped) continue;
      if (f.size > kMaxFileBytes) {
        r.notes.push_back(f.name + " was not read: larger than 256 MB; split the export "
                          "(bq extract with a wildcard URI writes several files)");
        partial = true;
        continue;
      }
      if (const std::string err = array_structure_error(f.path); !err.empty()) {
        r.notes.push_back(f.name + " was not read: " + err);
        partial = true;
        continue;
      }
      if (f.size <= raw_limit) {
        if (auto bytes = read_whole(f.path, f.size)) {
          const std::string ref = sink.put_raw("firebase", "exports", f.name, *bytes);
          if (ref.empty()) {
            r.notes.push_back(f.name + ": the raw copy could not be stored");
          } else {
            raw_refs[f.name] = ref;
            r.raw_written++;
          }
        }
      } else {
        r.notes.push_back(f.name + ": no raw copy kept (larger than " +
                          std::to_string(raw_limit / (1024 * 1024)) + " MB); signals cite it by name");
      }

      RowReader reader(f.path);
      std::string row;
      std::int64_t bad = 0, first_bad = 0, good = 0;
      while (reader.next(row)) {
        if (agg.stats.rows >= kMaxRowsRead) {
          capped = true;
          break;
        }
        json::ParseError perr;
        auto v = json::parse(row, &perr);
        if (!v || !v->is_object()) {
          if (bad++ == 0) first_bad = reader.number();
          continue;
        }
        good++;
        agg.add(*v, f.name);
        if ((good & 0xFFFF) == 0 && ctx.cancel.cancelled()) {
          r.status = signals::SyncStatus::kFailed;
          r.error = "cancelled before any signal was written";
          return r;
        }
      }
      r.pages++;
      if (bad > 0) {
        partial = true;
        const std::string where = reader.array_mode() ? "element " : "line ";
        if (good == 0) {
          r.notes.push_back(f.name + " was not read: neither NDJSON (one JSON object per line) "
                            "nor a JSON array of objects (first problem at " + where +
                            std::to_string(first_bad) + ")");
        } else {
          r.notes.push_back(f.name + ": " + std::to_string(bad) + " row(s) are not JSON objects "
                            "(first at " + where + std::to_string(first_bad) +
                            "); the other rows were imported");
        }
      }
    }
    if (capped) {
      partial = true;
      r.notes.push_back("stopped reading after " + std::to_string(kMaxRowsRead) +
                        " rows; counts and percentiles cover only the rows read. Export a "
                        "narrower date range.");
    }
    note_stats(agg.stats, s, r.notes);
    note_file_changes(cursor, seen, r.notes);

    // Build, then write the largest first, so a capped sync keeps what
    // matters most.
    std::vector<Built> crash, perf;
    for (const auto& [key, g] : agg.crashes) crash.push_back(build_crash(g, raw_refs));
    for (const auto& [key, g] : agg.perf) perf.push_back(build_perf(g, key, raw_refs));
    auto by_weight = [](const Built& x, const Built& y) { return x.weight > y.weight; };
    std::stable_sort(crash.begin(), crash.end(), by_weight);
    std::stable_sort(perf.begin(), perf.end(), by_weight);
    std::vector<Built*> order;
    for (auto& b : crash) order.push_back(&b);
    for (auto& b : perf) order.push_back(&b);

    std::size_t failures = 0;
    std::size_t considered = 0;
    for (Built* b : order) {
      if (r.records_written >= ctx.max_records) break;
      considered++;
      signals::SignalRecord& rec = b->record;
      rec.provider = "firebase";
      rec.connector_id = config.id;
      rec.workspace_id = ctx.workspace_id;
      rec.observed_at = ctx.now_iso;
      rec.id = stable_signal_id(config.id, rec.external_id);
      if (capped) rec.attributes.set("rows_capped", Value::boolean(true));
      std::string err;
      if (sink.put_signal(rec, &err)) {
        r.records_written++;
      } else {
        partial = true;
        if (failures++ < kMaxNotedFailures) r.notes.push_back(rec.external_id + " not stored: " + err);
      }
    }
    if (considered < order.size()) {
      partial = true;
      r.notes.push_back(std::to_string(order.size() - considered) +
                        " more signal(s) not written: this sync's limit is " +
                        std::to_string(ctx.max_records) + " (crashes first, most events first)");
    }

    r.cursor.extra.set("files", std::move(seen));
    // The watermark only moves when every file was read in full.
    if (!partial && agg.max_us) {
      const std::string mark = iso_from_us(*agg.max_us);
      const auto prev = signals::parse_iso8601(cursor.watermark);
      if (!prev || *agg.max_us / 1'000'000 > *prev) r.cursor.watermark = mark;
    }
    r.status = partial ? signals::SyncStatus::kPartial : signals::SyncStatus::kComplete;
    return r;
  }

 private:
  static void note_stats(const Stats& st, const Settings& s, std::vector<std::string>& notes) {
    if (st.duplicates > 0) {
      notes.push_back(std::to_string(st.duplicates) +
                      " Crashlytics row(s) repeat an event_id already read (a batch table and "
                      "its _REALTIME twin, or the same export twice) and were counted once");
    }
    if (st.other > 0) {
      notes.push_back(std::to_string(st.other) +
                      " row(s) are neither Crashlytics events nor Performance events and were "
                      "ignored");
    }
    if (st.filtered_kind > 0) {
      notes.push_back(std::to_string(st.filtered_kind) + " row(s) skipped: dataset_kind is " + s.kind);
    }
    if (st.filtered_app > 0) {
      notes.push_back(std::to_string(st.filtered_app) + " row(s) skipped: not app " + s.app);
    }
    if (st.perf_unknown_app > 0) {
      notes.push_back(std::to_string(st.perf_unknown_app) +
                      " Performance row(s) name no app (the export has one table per app) and "
                      "were kept as " + s.app + "'s; export only that app's table");
    }
    if (st.negative > 0) {
      notes.push_back(std::to_string(st.negative) + " negative duration(s) ignored");
    }
    if (st.untimed > 0) {
      notes.push_back(std::to_string(st.untimed) +
                      " row(s) have no readable event_timestamp; they are counted but date nothing");
    }
  }

  static void note_file_changes(const signals::SyncCursor& before, const Value& now,
                                std::vector<std::string>& notes) {
    const Value* prev = before.extra.find("files");
    if (prev == nullptr || !prev->is_object()) return;
    int added = 0, changed = 0, same = 0;
    std::vector<std::string> gone;
    for (const auto& [name, meta] : now.members()) {
      const Value* old = prev->find(name);
      if (old == nullptr) {
        added++;
      } else if (old->dump() == meta.dump()) {
        same++;
      } else {
        changed++;
      }
    }
    for (const auto& [name, meta] : prev->members()) {
      if (now.find(name) == nullptr) gone.push_back(name);
    }
    notes.push_back("since the last sync: " + std::to_string(added) + " new, " +
                    std::to_string(changed) + " changed, " + std::to_string(same) +
                    " unchanged export file(s); all were re-read");
    if (!gone.empty()) {
      notes.push_back(std::to_string(gone.size()) + " file(s) read last time are gone (" + gone.front() +
                      (gone.size() > 1 ? ", ..." : "") +
                      "); signals only they contributed to keep their last counts");
    }
  }
};

}  // namespace

namespace firebase {

std::optional<std::int64_t> parse_timestamp_us(const json::Value& v) {
  if (v.is_int()) return epoch_int_to_us(v.as_int());
  if (v.is_double()) return epoch_real_to_us(v.as_double());
  if (!v.is_string() || v.as_string().empty()) return std::nullopt;
  const std::string& s = v.as_string();
  if (auto i = integer(&v)) return epoch_int_to_us(*i);
  // "1.7282E9": BigQuery's tabledata API writes TIMESTAMP as float seconds.
  if (s.find('-', 1) == std::string::npos && s.find(':') == std::string::npos) {
    if (auto d = real(&v)) return epoch_real_to_us(*d);
  }
  return parse_text_time_us(s);
}

double nearest_rank(const std::vector<double>& sorted, int p) {
  const std::size_t n = sorted.size();
  if (n == 0) return std::nan("");
  const auto pp = static_cast<std::size_t>(std::clamp(p, 0, 100));
  std::size_t rank = (pp * n + 99) / 100;  // ceil(p * n / 100)
  if (rank < 1) rank = 1;
  return sorted[rank - 1];
}

}  // namespace firebase

std::unique_ptr<signals::SignalConnector> make_firebase_connector() {
  return std::make_unique<FirebaseConnector>();
}

}  // namespace mpi::intelligence
