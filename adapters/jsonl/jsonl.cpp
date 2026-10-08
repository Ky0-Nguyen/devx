#include "adapters/jsonl/jsonl.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace mpi::intelligence {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr std::uint64_t kMaxFileBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxRawFileBytes = 64ull * 1024ull * 1024ull;
// The store refuses a record over 4 MB; a longer line cannot become one.
constexpr std::size_t kMaxLineBytes = 4u * 1024u * 1024u;
constexpr std::int64_t kMaxLinesRead = 5'000'000;

std::string text(const Value& o, const char* key) {
  const Value* v = o.find(key);
  if (v == nullptr) return {};
  if (v->is_string()) return v->as_string();
  if (v->is_int()) return std::to_string(v->as_int());
  return {};
}

std::optional<std::string> opt_text(const Value& o, const char* key) {
  std::string s = text(o, key);
  if (s.empty()) return std::nullopt;
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

// make_signal_id cuts ids at 64 characters, so two long external ids with a
// common prefix would share a file and overwrite each other. When the id
// would be cut, its tail becomes a hash of the whole external id: stable
// across imports, distinct between records.
std::string stable_signal_id(const std::string& connector_id, const std::string& external_id) {
  std::string id = signals::make_signal_id(connector_id, external_id);
  if (id.size() < 64) return id;
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx",
                static_cast<unsigned long long>(fnv1a(connector_id + "\n" + external_id)));
  return id.substr(0, 64 - 17) + "_" + buf;
}

// A kind is a short lowercase word (crash, issue, metric, deploy, ...): it
// becomes part of queries and of the UI, so free text is refused.
bool kind_is_safe(const std::string& k) {
  if (k.empty() || k.size() > 32) return false;
  return std::all_of(k.begin(), k.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

// "Exact" means a deterministic identity on both sides (signal.hpp): a full
// commit SHA, or version + build + app identifier together.
bool supports_exact(const signals::ReleaseIdentity& r) {
  if (r.commit_sha && signals::looks_like_sha(*r.commit_sha) &&
      (r.commit_sha->size() == 40 || r.commit_sha->size() == 64)) {
    return true;
  }
  return r.version && r.build_number && r.bundle_or_package_id;
}

struct InputFile {
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

bool stat_regular(const std::string& path, InputFile* f) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  f->size = static_cast<std::uint64_t>(st.st_size);
  f->mtime = static_cast<std::int64_t>(st.st_mtime);
  return true;
}

// The file `path` names, or the *.jsonl / *.ndjson files of the directory it
// names, sorted so every import reads them in the same order.
std::vector<InputFile> list_inputs(const std::string& path, std::string* error) {
  std::vector<InputFile> out;
  std::error_code ec;
  if (path.empty()) {
    *error = "path is required: a .jsonl file or a directory of them";
    return out;
  }
  if (!fs::exists(path, ec)) {
    *error = "path " + path + " does not exist";
    return out;
  }
  if (!fs::is_directory(path, ec)) {
    InputFile f;
    f.name = fs::path(path).filename().string();
    f.path = path;
    if (!stat_regular(path, &f)) {
      *error = "path " + path + " is not a regular file";
      return out;
    }
    out.push_back(std::move(f));
    return out;
  }
  fs::directory_iterator it(path, ec);
  if (ec) {
    *error = "path " + path + " cannot be read: " + ec.message();
    return out;
  }
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    InputFile f;
    f.name = it->path().filename().string();
    if (f.name.empty() || f.name[0] == '.') continue;
    std::string lname = f.name;
    std::transform(lname.begin(), lname.end(), lname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ends_with(lname, ".jsonl") && !ends_with(lname, ".ndjson")) continue;
    f.path = it->path().string();
    if (!stat_regular(f.path, &f)) continue;
    out.push_back(std::move(f));
  }
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) { return a.name < b.name; });
  return out;
}

std::optional<std::string> read_whole(const std::string& path, std::uint64_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string s(static_cast<std::size_t>(size), '\0');
  in.read(s.data(), static_cast<std::streamsize>(size));
  if (static_cast<std::uint64_t>(in.gcount()) != size) return std::nullopt;
  return s;
}

// One line to a record, or nullopt with the reason.
std::optional<signals::SignalRecord> record_from_line(const Value& v, bool* claimed_exact,
                                                      std::string* why) {
  *claimed_exact = false;
  if (!v.is_object()) {
    *why = "not a JSON object";
    return std::nullopt;
  }
  if (v.find("schema_version") != nullptr) {
    // A full devx.signal/1 record, validated exactly as the store reads one.
    auto r = signals::SignalRecord::from_json(v, why);
    if (!r) return std::nullopt;
    if (!kind_is_safe(r->kind)) {
      *why = "kind '" + r->kind + "' is not a short lowercase word";
      return std::nullopt;
    }
    *claimed_exact = r->basis == signals::EvidenceBasis::kExact;
    return r;
  }
  signals::SignalRecord r;
  r.kind = text(v, "kind");
  if (!kind_is_safe(r.kind)) {
    *why = r.kind.empty() ? "no kind" : "kind '" + r.kind + "' is not a short lowercase word";
    return std::nullopt;
  }
  r.external_id = text(v, "external_id");
  if (r.external_id.empty()) r.external_id = text(v, "id");
  if (r.external_id.empty()) {
    *why = "no external_id or id";
    return std::nullopt;
  }
  r.occurred_at = text(v, "occurred_at");
  r.title = opt_text(v, "title");
  r.severity = opt_text(v, "severity");
  r.environment = opt_text(v, "environment");
  if (const Value* rel = v.find("release"); rel != nullptr && rel->is_object()) {
    r.release = signals::ReleaseIdentity::from_json(*rel);
    // Whoever wrote the file asserted these facts, unless the line says
    // where they came from.
    if (rel->find("source") == nullptr) r.release.source = signals::FactSource::kUserAsserted;
  }
  if (const Value* a = v.find("attributes"); a != nullptr && a->is_object()) r.attributes = *a;
  const std::string basis = text(v, "basis");
  if (!basis.empty()) {
    r.basis = signals::basis_from_string(basis);
  } else {
    // A release named by the line is the line's own claim about itself.
    r.basis = r.release.empty() ? signals::EvidenceBasis::kUnknown
                                : signals::EvidenceBasis::kProviderAttributed;
  }
  *claimed_exact = r.basis == signals::EvidenceBasis::kExact;
  return r;
}

class JsonlConnector : public signals::SignalConnector {
 public:
  signals::ConnectorInfo info() const override {
    signals::ConnectorInfo i;
    i.provider = "jsonl";
    i.display_name = "JSONL import";
    i.category = "import";
    i.egress = "Nothing: reads a file on this Mac";
    i.settings = {
        {"path",
         "Required. A .jsonl file, or a directory of .jsonl / .ndjson files. One JSON object per "
         "line: a devx.signal/1 record, or {kind, external_id, occurred_at, title, severity, "
         "environment, release{...}, attributes{...}, basis}."},
        {"provider_label",
         "Optional. The tool the file came from, stored as attributes.source_provider."},
    };
    return i;
  }

  signals::ConnectorCapabilities capabilities() const override {
    signals::ConnectorCapabilities c;
    c.issues = true;
    c.metrics = true;
    c.events = true;
    c.network = false;
    return c;
  }

  signals::ValidateResult validate(const signals::ConnectorConfig& config,
                                   const signals::ConnectorContext&) override {
    signals::ValidateResult r;
    const auto files = list_inputs(expand_home(text(config.settings, "path")), &r.error);
    if (!r.error.empty()) return r;
    r.notes.push_back(std::to_string(files.size()) + " file(s) to import");
    r.notes.push_back("reads local files only; nothing leaves this Mac");
    r.ok = true;
    return r;
  }

  signals::DiscoverResult discover(const signals::ConnectorConfig& config,
                                   const signals::ConnectorContext&) override {
    signals::DiscoverResult r;
    const auto files = list_inputs(expand_home(text(config.settings, "path")), &r.error);
    if (!r.error.empty()) return r;
    for (const auto& f : files) {
      Value one = Value::object();
      one.set("id", Value::string(f.name));
      one.set("name", Value::string(f.name));
      one.set("kind", Value::string("jsonl"));
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
    const auto files = list_inputs(expand_home(text(config.settings, "path")), &r.error);
    if (!r.error.empty()) {
      r.status = signals::SyncStatus::kFailed;
      return r;
    }
    const std::string label = text(config.settings, "provider_label");
    const std::uint64_t raw_limit =
        std::min<std::uint64_t>(kMaxRawFileBytes, static_cast<std::uint64_t>(ctx.max_raw_bytes));

    bool partial = false, stopped = false;
    std::int64_t lines_read = 0, downgraded = 0, duplicates = 0;
    std::optional<std::int64_t> newest;
    std::set<std::string> written_ids;
    Value seen = Value::object();
    for (const auto& f : files) {
      Value meta = Value::object();
      meta.set("size", Value::integer(static_cast<std::int64_t>(f.size)));
      meta.set("mtime", Value::string(signals::format_iso8601(f.mtime)));
      seen.set(f.name, std::move(meta));
      if (stopped) continue;
      if (ctx.cancel.cancelled()) {
        partial = stopped = true;
        r.notes.push_back("cancelled; the files after " + f.name + " were not read");
        continue;
      }
      if (f.size > kMaxFileBytes) {
        partial = true;
        r.notes.push_back(f.name + " was not read: larger than 256 MB");
        continue;
      }
      std::string raw_ref;
      if (f.size <= raw_limit) {
        if (auto bytes = read_whole(f.path, f.size)) {
          raw_ref = sink.put_raw("jsonl", "imports", f.name, *bytes);
          if (raw_ref.empty()) {
            r.notes.push_back(f.name + ": the raw copy could not be stored");
          } else {
            r.raw_written++;
          }
        }
      } else {
        r.notes.push_back(f.name + ": no raw copy kept (larger than " +
                          std::to_string(raw_limit / (1024 * 1024)) + " MB)");
      }

      std::ifstream in(f.path, std::ios::binary);
      std::string line;
      std::int64_t number = 0, bad = 0;
      std::string first_problem;
      while (std::getline(in, line)) {
        number++;
        if (++lines_read > kMaxLinesRead) {
          partial = stopped = true;
          r.notes.push_back("stopped after " + std::to_string(kMaxLinesRead) + " lines");
          break;
        }
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        auto problem = [&](const std::string& why) {
          if (bad++ == 0) first_problem = "line " + std::to_string(number) + ": " + why;
        };
        if (line.size() > kMaxLineBytes) {
          problem("longer than 4 MB");
          continue;
        }
        json::ParseError perr;
        auto v = json::parse(line, &perr);
        if (!v) {
          problem("not JSON (" + perr.message + ")");
          continue;
        }
        bool claimed_exact = false;
        std::string why;
        auto rec = record_from_line(*v, &claimed_exact, &why);
        if (!rec) {
          problem(why);
          continue;
        }
        if (!rec->occurred_at.empty() && !signals::parse_iso8601(rec->occurred_at)) {
          problem("occurred_at '" + rec->occurred_at + "' is not ISO 8601");
          continue;
        }
        if (claimed_exact && !supports_exact(rec->release)) {
          // Certainty cannot be imported: it has to rest on an identity
          // that both sides can check.
          rec->basis = signals::EvidenceBasis::kProviderAttributed;
          rec->attributes.set("basis_downgraded_from", Value::string("exact"));
          downgraded++;
        }
        // The configured connector owns what it imports, whatever the line
        // says; where the line came from is kept as an attribute.
        if (!label.empty()) {
          rec->attributes.set("source_provider", Value::string(label));
        } else if (!rec->provider.empty() && rec->provider != "jsonl") {
          rec->attributes.set("source_provider", Value::string(rec->provider));
        }
        rec->provider = "jsonl";
        rec->connector_id = config.id;
        rec->workspace_id = ctx.workspace_id;
        rec->observed_at = ctx.now_iso;
        rec->id = stable_signal_id(config.id, rec->external_id);
        rec->raw_ref = raw_ref;
        rec->attributes.set("source_file", Value::string(f.name));
        rec->attributes.set("source_line", Value::integer(number));
        if (r.records_written >= ctx.max_records && written_ids.count(rec->id) == 0) {
          partial = stopped = true;
          r.notes.push_back("stopped at this sync's limit of " + std::to_string(ctx.max_records) +
                            " records, in " + f.name + " at line " + std::to_string(number));
          break;
        }
        std::string err;
        if (!sink.put_signal(*rec, &err)) {
          problem("not stored: " + err);
          continue;
        }
        if (!written_ids.insert(rec->id).second) {
          duplicates++;
        } else {
          r.records_written++;
        }
        if (auto t = signals::parse_iso8601(rec->occurred_at); t && (!newest || *t > *newest)) newest = t;
      }
      r.pages++;
      if (bad > 0) {
        partial = true;
        r.notes.push_back(f.name + ": " + std::to_string(bad) + " line(s) not imported (first at " +
                          first_problem + "); the other lines were");
      }
    }
    if (files.empty()) r.notes.push_back("no .jsonl or .ndjson files to import yet");
    if (downgraded > 0) {
      r.notes.push_back(std::to_string(downgraded) +
                        " line(s) claimed basis exact without a full commit SHA or version + "
                        "build + app identifier; stored as provider_attributed");
    }
    if (duplicates > 0) {
      r.notes.push_back(std::to_string(duplicates) +
                        " line(s) repeat an external id already imported; the later line wins");
    }
    r.cursor.extra.set("files", std::move(seen));
    if (!partial && newest) {
      const auto prev = signals::parse_iso8601(cursor.watermark);
      if (!prev || *newest > *prev) r.cursor.watermark = signals::format_iso8601(*newest);
    }
    r.status = partial ? signals::SyncStatus::kPartial : signals::SyncStatus::kComplete;
    return r;
  }
};

}  // namespace

std::unique_ptr<signals::SignalConnector> make_jsonl_connector() {
  return std::make_unique<JsonlConnector>();
}

}  // namespace mpi::intelligence
