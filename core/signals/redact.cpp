#include "core/signals/redact.hpp"

#include <map>
#include <regex>

namespace mpi::signals {
namespace {

struct Rule {
  const char* kind;
  std::regex pattern;
  /// The capture group that is the secret; the rest of the match is kept so
  /// the excerpt still reads ("Authorization: Bearer [redacted:...]").
  int group;
};

const std::vector<Rule>& rules(bool pii, bool ip) {
  static const auto flags = std::regex::ECMAScript | std::regex::optimize;
  static const std::vector<Rule> secrets = {
      {"private_key",
       std::regex("(-----BEGIN [A-Z ]*PRIVATE KEY-----[\\s\\S]*?-----END [A-Z ]*PRIVATE KEY-----)", flags), 1},
      {"bearer_token", std::regex("([Bb]earer\\s+)([A-Za-z0-9._~+/=-]{8,})", flags), 2},
      {"basic_auth", std::regex("([Bb]asic\\s+)([A-Za-z0-9+/=]{8,})", flags), 2},
      {"jwt", std::regex("(eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,})", flags), 1},
      {"gitlab_token", std::regex("(glpat-[A-Za-z0-9_-]{16,})", flags), 1},
      {"github_token", std::regex("((?:ghp|gho|ghu|ghs|ghr|github_pat)_[A-Za-z0-9_]{16,})", flags), 1},
      {"sentry_token", std::regex("(sntry[su]_[A-Za-z0-9_=+/-]{16,})", flags), 1},
      {"slack_token", std::regex("(xox[abprs]-[A-Za-z0-9-]{10,})", flags), 1},
      {"aws_access_key", std::regex("((?:AKIA|ASIA)[A-Z0-9]{16})", flags), 1},
      {"google_api_key", std::regex("(AIza[0-9A-Za-z_-]{35})", flags), 1},
      {"url_credentials", std::regex("(https?://[^/\\s:@]+:)([^/\\s@]+)(@)", flags), 2},
      // key=value and "key": "value" where the key names a secret. A value
      // starting with '[' is a marker an earlier rule left: not re-redacted,
      // so the excerpt keeps saying what kind of secret was there.
      {"secret_assignment",
       std::regex("((?:password|passwd|secret|token|api[_-]?key|access[_-]?key|auth[_-]?token|"
                   "private[_-]?key|client[_-]?secret)[\"']?\\s*[:=]\\s*[\"']?)([^\\s\"',;&\\[][^\\s\"',;&]{3,})",
                   flags | std::regex::icase),
       2},
  };
  static const std::vector<Rule> with_email = [] {
    std::vector<Rule> r = secrets;
    r.push_back({"email", std::regex("([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,})", flags), 1});
    return r;
  }();
  static const std::vector<Rule> with_pii = [] {
    std::vector<Rule> r = with_email;
    r.push_back({"ip_address",
                 std::regex("(\\b(?:(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)\\.){3}(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)\\b)",
                            flags),
                 1});
    return r;
  }();
  if (!pii) return secrets;
  return ip ? with_pii : with_email;
}

}  // namespace

int Redaction::total() const {
  int n = 0;
  for (const auto& [_, c] : removed) n += c;
  return n;
}

Redaction redact(const std::string& text, bool pii, bool ip_addresses) {
  Redaction out;
  out.text = text;
  std::map<std::string, int> counts;
  for (const Rule& rule : rules(pii, ip_addresses)) {
    std::string result;
    auto begin = std::sregex_iterator(out.text.begin(), out.text.end(), rule.pattern);
    auto end = std::sregex_iterator();
    std::size_t last = 0;
    int n = 0;
    for (auto it = begin; it != end; ++it) {
      const std::smatch& m = *it;
      const auto g_pos = static_cast<std::size_t>(m.position(static_cast<std::size_t>(rule.group)));
      const auto g_len = static_cast<std::size_t>(m.length(static_cast<std::size_t>(rule.group)));
      result.append(out.text, last, g_pos - last);
      result += std::string("[redacted:") + rule.kind + "]";
      last = g_pos + g_len;
      n++;
    }
    if (n == 0) continue;
    result.append(out.text, last, std::string::npos);
    out.text = std::move(result);
    counts[rule.kind] += n;
  }
  for (const auto& [k, v] : counts) out.removed.emplace_back(k, v);
  return out;
}

json::Value redact_json(const json::Value& v, bool pii, bool ip_addresses,
                        std::map<std::string, int>* counts) {
  switch (v.type()) {
    case json::Value::Type::String: {
      const Redaction r = redact(v.as_string(), pii, ip_addresses);
      if (counts != nullptr) {
        for (const auto& [k, n] : r.removed) (*counts)[k] += n;
      }
      return json::Value::string(r.text);
    }
    case json::Value::Type::Array: {
      json::Value a = json::Value::array();
      for (const auto& item : v.items()) a.push_back(redact_json(item, pii, ip_addresses, counts));
      return a;
    }
    case json::Value::Type::Object: {
      json::Value o = json::Value::object();
      for (const auto& [k, item] : v.members()) o.set(k, redact_json(item, pii, ip_addresses, counts));
      return o;
    }
    default:
      return v;
  }
}

}  // namespace mpi::signals
