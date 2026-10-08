// Redaction before evidence reaches a model (section 18).
//
// Raw evidence stays on disk as the provider sent it -- it is the audit
// trail. What leaves it for an AI host (an MCP excerpt, an evidence pack)
// passes through here first: credentials that CI logs and HTTP payloads are
// known to carry are replaced with a marker naming what was removed, and
// with `pii` on, e-mail addresses and IP addresses too.
//
// This is a scanner for known shapes, not a guarantee: a secret in a shape
// it does not know passes through. That is why raw views still warn, and
// why the raw store never leaves the machine by itself.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::signals {

struct Redaction {
  std::string text;
  /// What was replaced, by kind and count: {"bearer_token": 2, ...}.
  std::vector<std::pair<std::string, int>> removed;
  int total() const;
};

/// `ip_addresses` is separate from `pii` because a four-part version number
/// looks exactly like one: on a whole evidence pack, where release versions
/// matter, it is left off.
Redaction redact(const std::string& text, bool pii = true, bool ip_addresses = true);

/// Redacts every string in a JSON tree, keys left alone, adding what it
/// removed to `counts`. Walking the tree rather than its serialised text
/// means a rule can never break the JSON -- an escaped quote inside a value
/// is just a character here -- so there is no failure path that could hand
/// back the unredacted document.
json::Value redact_json(const json::Value& v, bool pii, bool ip_addresses,
                        std::map<std::string, int>* counts);

}  // namespace mpi::signals
