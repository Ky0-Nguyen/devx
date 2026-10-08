#include "core/signals/signal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace mpi::signals {
namespace {

std::optional<std::string> opt_str(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  if (v == nullptr || !v->is_string() || v->as_string().empty()) return std::nullopt;
  return v->as_string();
}

std::string str(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

void set_opt(json::Value& o, const char* key, const std::optional<std::string>& v) {
  if (v) o.set(key, json::Value::string(*v));
}

std::vector<std::string> str_list(const json::Value& o, const char* key) {
  std::vector<std::string> out;
  const json::Value* v = o.find(key);
  if (v == nullptr || !v->is_array()) return out;
  for (const auto& item : v->items()) {
    if (item.is_string() && !item.as_string().empty()) out.push_back(item.as_string());
  }
  return out;
}

json::Value list_json(const std::vector<std::string>& xs) {
  json::Value a = json::Value::array();
  for (const auto& x : xs) a.push_back(json::Value::string(x));
  return a;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// The part of an external id that may appear in a file name.
std::string sanitize(const std::string& s) {
  std::string out;
  for (char c : lower(s)) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
    if (out.size() >= 80) break;
  }
  return out;
}

}  // namespace

const char* to_string(EvidenceBasis b) {
  switch (b) {
    case EvidenceBasis::kExact: return "exact";
    case EvidenceBasis::kProviderAttributed: return "provider_attributed";
    case EvidenceBasis::kCandidate: return "candidate";
    case EvidenceBasis::kUnknown: return "unknown";
  }
  return "unknown";
}

EvidenceBasis basis_from_string(const std::string& s) {
  if (s == "exact") return EvidenceBasis::kExact;
  if (s == "provider_attributed") return EvidenceBasis::kProviderAttributed;
  // "temporal_candidate" is the spec's longer spelling of the same thing.
  if (s == "candidate" || s == "temporal_candidate") return EvidenceBasis::kCandidate;
  return EvidenceBasis::kUnknown;
}

const char* to_string(FactSource s) {
  switch (s) {
    case FactSource::kProvider: return "provider";
    case FactSource::kBuildManifest: return "build_manifest";
    case FactSource::kGit: return "git";
    case FactSource::kUserAsserted: return "user_asserted";
    case FactSource::kDeviceProvider: return "device_provider";
    case FactSource::kUnknown: return "unknown";
  }
  return "unknown";
}

FactSource fact_source_from_string(const std::string& s) {
  if (s == "provider") return FactSource::kProvider;
  if (s == "build_manifest") return FactSource::kBuildManifest;
  if (s == "git") return FactSource::kGit;
  if (s == "user_asserted") return FactSource::kUserAsserted;
  if (s == "device_provider") return FactSource::kDeviceProvider;
  return FactSource::kUnknown;
}

bool ReleaseIdentity::empty() const {
  return !version && !build_number && !commit_sha && !branch && !environment &&
         !bundle_or_package_id;
}

json::Value ReleaseIdentity::to_json() const {
  json::Value o = json::Value::object();
  set_opt(o, "version", version);
  set_opt(o, "build_number", build_number);
  set_opt(o, "commit_sha", commit_sha);
  set_opt(o, "branch", branch);
  set_opt(o, "environment", environment);
  set_opt(o, "bundle_or_package_id", bundle_or_package_id);
  o.set("source", json::Value::string(to_string(source)));
  return o;
}

ReleaseIdentity ReleaseIdentity::from_json(const json::Value& v) {
  ReleaseIdentity r;
  if (!v.is_object()) return r;
  r.version = opt_str(v, "version");
  r.build_number = opt_str(v, "build_number");
  if (auto sha = opt_str(v, "commit_sha")) r.commit_sha = lower(*sha);
  r.branch = opt_str(v, "branch");
  r.environment = opt_str(v, "environment");
  r.bundle_or_package_id = opt_str(v, "bundle_or_package_id");
  r.source = fact_source_from_string(str(v, "source"));
  return r;
}

std::string ReleaseIdentity::key() const {
  if (version) {
    std::string k;
    if (bundle_or_package_id) k += *bundle_or_package_id + "@";
    k += *version;
    if (build_number) k += "+" + *build_number;
    return k;
  }
  if (commit_sha) return "commit:" + *commit_sha;
  return {};
}

json::Value SignalRecord::to_json() const {
  json::Value o = json::Value::object();
  o.set("schema_version", json::Value::string(schema_version));
  o.set("id", json::Value::string(id));
  o.set("workspace_id", json::Value::string(workspace_id));
  o.set("provider", json::Value::string(provider));
  o.set("connector_id", json::Value::string(connector_id));
  o.set("kind", json::Value::string(kind));
  o.set("external_id", json::Value::string(external_id));
  o.set("occurred_at", json::Value::string(occurred_at));
  o.set("observed_at", json::Value::string(observed_at));
  set_opt(o, "environment", environment);
  set_opt(o, "severity", severity);
  set_opt(o, "title", title);
  o.set("release", release.to_json());
  o.set("attributes", attributes.is_object() ? attributes : json::Value::object());
  o.set("raw_ref", json::Value::string(raw_ref));
  o.set("basis", json::Value::string(to_string(basis)));
  return o;
}

std::optional<SignalRecord> SignalRecord::from_json(const json::Value& v, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<SignalRecord> {
    if (error != nullptr) *error = why;
    return std::nullopt;
  };
  if (!v.is_object()) return fail("a signal is a JSON object");
  SignalRecord r;
  r.schema_version = str(v, "schema_version");
  if (r.schema_version != kSignalSchema) {
    return fail("unsupported signal schema '" + r.schema_version + "'");
  }
  r.id = str(v, "id");
  r.workspace_id = str(v, "workspace_id");
  r.provider = str(v, "provider");
  r.connector_id = str(v, "connector_id");
  r.kind = str(v, "kind");
  r.external_id = str(v, "external_id");
  if (r.id.empty() || r.provider.empty() || r.connector_id.empty() || r.kind.empty() ||
      r.external_id.empty()) {
    return fail("a signal needs id, provider, connector_id, kind and external_id");
  }
  if (!id_is_safe(r.id)) return fail("not a safe signal id: '" + r.id + "'");
  r.occurred_at = str(v, "occurred_at");
  r.observed_at = str(v, "observed_at");
  r.environment = opt_str(v, "environment");
  r.severity = opt_str(v, "severity");
  r.title = opt_str(v, "title");
  if (const json::Value* rel = v.find("release")) r.release = ReleaseIdentity::from_json(*rel);
  if (const json::Value* a = v.find("attributes"); a != nullptr && a->is_object()) r.attributes = *a;
  r.raw_ref = str(v, "raw_ref");
  r.basis = basis_from_string(str(v, "basis"));
  return r;
}

std::string make_signal_id(const std::string& connector_id, const std::string& external_id) {
  const std::string conn = sanitize(connector_id), ext = sanitize(external_id);
  std::string id = "sig_" + conn + "_" + ext;
  // Sanitising is lossy -- case, and every character outside [a-z0-9_-] --
  // so "ABC-1" and "abc-1", or "1.2.0+45" and "1.2.0_45", would share a file
  // and one would silently overwrite the other. Whenever it changed
  // anything, or the id is too long to keep whole, the id ends in a hash of
  // the exact pair (FNV-1a, 64-bit: stable across runs and builds). A ':' is
  // the one separator connectors use in their own ids ("pipeline:9321"), and
  // it is never part of a provider's value, so it alone is not a loss.
  auto lossless = [](const std::string& in, const std::string& out) {
    if (in.size() != out.size()) return false;
    for (std::size_t i = 0; i < in.size(); i++) {
      if (in[i] != out[i] && !(in[i] == ':' && out[i] == '_')) return false;
    }
    return true;
  };
  if (id.size() <= 64 && lossless(connector_id, conn) && lossless(external_id, ext)) return id;
  std::uint64_t h = 1469598103934665603ull;
  for (const char c : connector_id + "\x1f" + external_id) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  return id.substr(0, 46) + "_" + hex;
}

json::Value RetentionPolicy::to_json() const {
  json::Value o = json::Value::object();
  o.set("days", json::Value::integer(days));
  return o;
}

RetentionPolicy RetentionPolicy::from_json(const json::Value& v) {
  RetentionPolicy p;
  if (const json::Value* d = v.find("days"); d != nullptr && d->is_number()) {
    p.days = std::max<int>(0, static_cast<int>(d->as_int()));
  }
  return p;
}

json::Value ProjectWorkspace::to_json() const {
  json::Value o = json::Value::object();
  o.set("schema", json::Value::string(kWorkspaceSchema));
  o.set("id", json::Value::string(id));
  o.set("name", json::Value::string(name));
  o.set("repository_root", json::Value::string(repository_root));
  o.set("app_identifiers", list_json(app_identifiers));
  o.set("environments", list_json(environments));
  o.set("retention", retention.to_json());
  return o;
}

std::optional<ProjectWorkspace> ProjectWorkspace::from_json(const json::Value& v,
                                                            std::string* error) {
  ProjectWorkspace w;
  w.id = str(v, "id");
  if (!id_is_safe(w.id)) {
    if (error != nullptr) *error = "a workspace id is [a-z0-9_-], up to 64 characters";
    return std::nullopt;
  }
  w.name = str(v, "name");
  if (w.name.empty()) w.name = w.id;
  w.repository_root = str(v, "repository_root");
  w.app_identifiers = str_list(v, "app_identifiers");
  w.environments = str_list(v, "environments");
  if (const json::Value* r = v.find("retention")) w.retention = RetentionPolicy::from_json(*r);
  return w;
}

json::Value ConnectorConfig::to_json() const {
  json::Value o = json::Value::object();
  o.set("schema", json::Value::string(kConnectorSchema));
  o.set("id", json::Value::string(id));
  o.set("provider", json::Value::string(provider));
  o.set("name", json::Value::string(name));
  o.set("settings", settings.is_object() ? settings : json::Value::object());
  o.set("credential_ref", json::Value::string(credential_ref));
  if (retention) o.set("retention", retention->to_json());
  o.set("paused", json::Value::boolean(paused));
  return o;
}

std::optional<ConnectorConfig> ConnectorConfig::from_json(const json::Value& v,
                                                          std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<ConnectorConfig> {
    if (error != nullptr) *error = why;
    return std::nullopt;
  };
  ConnectorConfig c;
  c.id = str(v, "id");
  c.provider = str(v, "provider");
  if (!id_is_safe(c.id)) return fail("a connector id is [a-z0-9_-], up to 64 characters");
  if (!id_is_safe(c.provider)) return fail("a connector needs a provider");
  c.name = str(v, "name");
  if (c.name.empty()) c.name = c.id;
  if (const json::Value* s = v.find("settings"); s != nullptr && s->is_object()) {
    for (const auto& [k, val] : s->members()) {
      // Settings are written to disk in the clear; a secret never belongs
      // there, and refusing it here is cheaper than finding it in a backup.
      if (key_looks_secret(k)) {
        return fail("setting '" + k + "' looks like a secret; credentials go in the "
                    "Keychain or the environment, never in connector settings");
      }
    }
    c.settings = *s;
  }
  c.credential_ref = str(v, "credential_ref");
  if (const json::Value* r = v.find("retention"); r != nullptr && r->is_object()) {
    c.retention = RetentionPolicy::from_json(*r);
  }
  if (const json::Value* p = v.find("paused")) c.paused = p->as_bool(false);
  return c;
}

json::Value SyncCursor::to_json() const {
  json::Value o = json::Value::object();
  o.set("connector_id", json::Value::string(connector_id));
  o.set("watermark", json::Value::string(watermark));
  o.set("resume", json::Value::string(resume));
  o.set("updated_at", json::Value::string(updated_at));
  o.set("extra", extra.is_object() ? extra : json::Value::object());
  return o;
}

SyncCursor SyncCursor::from_json(const json::Value& v) {
  SyncCursor c;
  c.connector_id = str(v, "connector_id");
  c.watermark = str(v, "watermark");
  c.resume = str(v, "resume");
  c.updated_at = str(v, "updated_at");
  if (const json::Value* e = v.find("extra"); e != nullptr && e->is_object()) c.extra = *e;
  return c;
}

const char* to_string(SyncStatus s) {
  switch (s) {
    case SyncStatus::kComplete: return "complete";
    case SyncStatus::kPartial: return "partial";
    case SyncStatus::kFailed: return "failed";
  }
  return "failed";
}

json::Value SyncResult::to_json() const {
  json::Value o = json::Value::object();
  o.set("status", json::Value::string(to_string(status)));
  o.set("records_written", json::Value::integer(records_written));
  o.set("raw_written", json::Value::integer(raw_written));
  o.set("pages", json::Value::integer(pages));
  o.set("rate_limited", json::Value::boolean(rate_limited));
  if (retry_after_s) o.set("retry_after_s", json::Value::integer(*retry_after_s));
  if (!error.empty()) o.set("error", json::Value::string(error));
  o.set("notes", list_json(notes));
  o.set("cursor", cursor.to_json());
  o.set("started_at", json::Value::string(started_at));
  o.set("finished_at", json::Value::string(finished_at));
  return o;
}

bool key_looks_secret(const std::string& key) {
  const std::string k = lower(key);
  for (const char* word : {"token", "password", "passwd", "secret", "dsn", "credential",
                           "private_key", "api_key", "apikey", "access_key", "auth"}) {
    if (k.find(word) != std::string::npos) return true;
  }
  return false;
}

bool looks_like_sha(const std::string& s) {
  if (s.size() < 7 || s.size() > 64) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool id_is_safe(const std::string& id) {
  if (id.empty() || id.size() > 64 || id[0] == '-') return false;
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

std::optional<std::int64_t> parse_iso8601(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (s.size() < 10 || std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return std::nullopt;
  if (s[4] != '-' || s[7] != '-') return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
  std::size_t i = 10;
  std::int64_t offset = 0;
  if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
    if (std::sscanf(s.c_str() + i + 1, "%2d:%2d", &h, &mi) != 2) return std::nullopt;
    i += 6;
    if (i < s.size() && s[i] == ':') {
      if (std::sscanf(s.c_str() + i + 1, "%2d", &sec) != 1) return std::nullopt;
      i += 3;
    }
    while (i < s.size() && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) i++;
    // What follows is a zone, and only a zone this can read: Z, UTC, or an
    // offset as +HH, +HHMM or +HH:MM. Anything else ("PST") is refused rather
    // than read as UTC.
    std::string zone = s.substr(i);
    while (!zone.empty() && zone.front() == ' ') zone.erase(zone.begin());
    if (zone == "Z" || zone == "UTC" || zone.empty()) {
      offset = 0;
    } else if (zone[0] == '+' || zone[0] == '-') {
      std::string digits;
      for (std::size_t k = 1; k < zone.size(); k++) {
        if (zone[k] >= '0' && zone[k] <= '9') {
          digits.push_back(zone[k]);
        } else if (!(zone[k] == ':' && k == 3)) {
          return std::nullopt;
        }
      }
      if (digits.size() != 2 && digits.size() != 4) return std::nullopt;
      const int oh = std::atoi(digits.substr(0, 2).c_str());
      const int om = digits.size() == 4 ? std::atoi(digits.substr(2).c_str()) : 0;
      if (oh > 23 || om > 59) return std::nullopt;
      offset = (zone[0] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    } else {
      return std::nullopt;
    }
  } else if (i < s.size()) {
    return std::nullopt;  // a date followed by something that is not a time
  }
  if (h > 23 || mi > 59 || sec > 60) return std::nullopt;
  // Days from civil (Howard Hinnant's algorithm), no time zone database.
  const int yy = y - (mo <= 2 ? 1 : 0);
  const int era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yy - era * 400);
  const unsigned doy = static_cast<unsigned>((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
  return days * 86400 + h * 3600 + mi * 60 + sec - offset;
}

std::string format_iso8601(std::int64_t epoch_s) {
  const std::time_t t = static_cast<std::time_t>(epoch_s);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace mpi::signals
