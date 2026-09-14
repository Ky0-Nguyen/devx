// A project's suppression list, on disk.
//
// A suppression is a project decision -- "we accept this finding, for this
// reason, until this date" -- so it belongs in a file the project keeps and
// reviews, not in one operator's application state. Both front ends read the
// same file, which is the only way the desktop app and a CI run can agree
// about what is suppressed.
//
// Three rules the format enforces, each because the opposite makes a
// suppression unauditable:
//
//   * **A reason is mandatory.** A suppression with no reason cannot be
//     reviewed, and one nobody can review is permanent by accident (H10).
//   * **An author is recorded**, so a reason has someone to ask about it.
//   * **An expiry is optional but real.** The engine does not apply a
//     suppression whose expiry has passed, and says which one lapsed. An
//     expiry that never expires would be worse than none.
#pragma once

#include <string>
#include <vector>

#include "core/rules/engine.hpp"

namespace mpi::session {

struct SuppressionEntry {
  std::string rule_id;
  // Empty means every finding from the rule. A fingerprint narrows it to one,
  // which is almost always what a project means.
  std::string fingerprint;
  std::string reason;
  std::string expiry;  // ISO-8601 date, or empty for none
  std::string author;
  std::string created_at;
  // Free-text pointer to wherever this was agreed: a ticket, a review.
  std::string reference;

  json::Value to_json() const;
};

struct SuppressionFile {
  std::string schema_version = "2.0";
  std::vector<SuppressionEntry> entries;

  json::Value to_json() const;
  // The entries as the engine wants them.
  std::vector<rules::EngineOptions::Suppression> to_engine() const;
};

struct SuppressionReadResult {
  bool ok = false;
  std::string error;
  // Entries the file contained but which were refused, with why. Refused
  // rather than skipped quietly: a malformed suppression that vanished would
  // look like a finding that was never suppressed.
  std::vector<std::string> rejected;
};

// Reads `path`. A missing file is **not** an error -- a project with no
// suppressions is the normal case -- and yields an empty list.
SuppressionReadResult read_suppressions(const std::string& path,
                                        SuppressionFile& out);

// Writes `path` atomically. Refuses an entry with no rule id or no reason
// rather than writing something that cannot be reviewed.
struct SuppressionWriteResult {
  bool ok = false;
  std::string error;
};
SuppressionWriteResult write_suppressions(const std::string& path,
                                          const SuppressionFile& file);

}  // namespace mpi::session
