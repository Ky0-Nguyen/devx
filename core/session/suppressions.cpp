#include "core/session/suppressions.hpp"

#include <cstdio>
#include <fstream>

#include "core/util/json.hpp"
#include "core/util/time.hpp"

namespace mpi::session {
namespace {

std::string str_at(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v != nullptr && v->is_string() ? v->as_string() : std::string();
}

}  // namespace

json::Value SuppressionEntry::to_json() const {
  json::Value o = json::Value::object();
  o.set("rule_id", json::Value::string(rule_id));
  o.set("fingerprint", json::Value::string(fingerprint));
  o.set("reason", json::Value::string(reason));
  o.set("expiry", json::Value::string(expiry));
  o.set("author", json::Value::string(author));
  o.set("created_at", json::Value::string(created_at));
  o.set("reference", json::Value::string(reference));
  return o;
}

json::Value SuppressionFile::to_json() const {
  json::Value o = json::Value::object();
  o.set("schema_version", json::Value::string(schema_version));
  json::Value arr = json::Value::array();
  for (const auto& e : entries) arr.push_back(e.to_json());
  o.set("suppressions", std::move(arr));
  return o;
}

std::vector<rules::EngineOptions::Suppression> SuppressionFile::to_engine() const {
  std::vector<rules::EngineOptions::Suppression> out;
  out.reserve(entries.size());
  for (const auto& e : entries) {
    rules::EngineOptions::Suppression s;
    s.rule_id = e.rule_id;
    s.fingerprint = e.fingerprint;
    s.reason = e.reason;
    s.expiry = e.expiry;
    s.author = e.author;
    out.push_back(std::move(s));
  }
  return out;
}

SuppressionReadResult read_suppressions(const std::string& path,
                                        SuppressionFile& out) {
  SuppressionReadResult res;
  out = SuppressionFile{};
  if (path.empty()) {
    res.ok = true;
    return res;
  }
  {
    std::ifstream probe(path);
    if (!probe) {
      // A project with no suppressions is the normal case, and the normal
      // case is not an error.
      res.ok = true;
      return res;
    }
  }
  json::ParseError perr;
  auto parsed = json::parse_file(path, json::Limits{}, &perr);
  if (!parsed) {
    res.error = path + ": " + perr.message;
    return res;
  }
  if (!parsed->is_object()) {
    res.error = path + ": expected an object at the top level";
    return res;
  }
  const std::string version = str_at(*parsed, "schema_version");
  if (!version.empty()) out.schema_version = version;

  const json::Value* arr = parsed->find("suppressions");
  if (arr == nullptr || !arr->is_array()) {
    res.error = path + ": no \"suppressions\" array";
    return res;
  }
  for (const auto& item : arr->items()) {
    if (!item.is_object()) {
      res.rejected.push_back("an entry that is not an object");
      continue;
    }
    SuppressionEntry e;
    e.rule_id = str_at(item, "rule_id");
    e.fingerprint = str_at(item, "fingerprint");
    e.reason = str_at(item, "reason");
    e.expiry = str_at(item, "expiry");
    e.author = str_at(item, "author");
    e.created_at = str_at(item, "created_at");
    e.reference = str_at(item, "reference");
    if (e.rule_id.empty()) {
      res.rejected.push_back("an entry with no rule_id");
      continue;
    }
    if (e.reason.empty()) {
      // The one rule worth refusing on: a suppression nobody can review is
      // permanent by accident (spec H10).
      res.rejected.push_back(e.rule_id + ": no reason, so it is not auditable");
      continue;
    }
    out.entries.push_back(std::move(e));
  }
  res.ok = true;
  return res;
}

SuppressionWriteResult write_suppressions(const std::string& path,
                                          const SuppressionFile& file) {
  SuppressionWriteResult res;
  if (path.empty()) {
    res.error = "no path was given";
    return res;
  }
  for (const auto& e : file.entries) {
    if (e.rule_id.empty()) {
      res.error = "refusing to write a suppression with no rule id";
      return res;
    }
    if (e.reason.empty()) {
      res.error = "refusing to write a suppression with no reason: one nobody "
                  "can review is permanent by accident";
      return res;
    }
  }
  // Written beside the target and renamed, so an interrupted write cannot
  // leave a project with a half-parsed suppression list.
  const std::string temp = path + ".partial";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f) {
      res.error = "cannot write " + temp;
      return res;
    }
    f << file.to_json().dump(2) << "\n";
    if (!f) {
      res.error = "failed while writing " + temp;
      return res;
    }
  }
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::remove(temp.c_str());
    res.error = "cannot move " + temp + " into place";
    return res;
  }
  res.ok = true;
  return res;
}

}  // namespace mpi::session
