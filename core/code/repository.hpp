// Code context from the local repository (section 15).
//
// Not an IDE indexer: the deterministic facts an agent needs to connect
// evidence to code -- which commit, what it changed, what a file said at that
// commit, who last touched a line -- read with `git` from the workspace's
// repository, argv-only (ADR-0003), every read bounded.
//
// A path a provider supplied (a stack frame's file) is resolved inside the
// repository and refused when it would leave it (path_is_within_root), and a
// ref is refused when git could read it as an option. When the repository is
// missing or moved, every call says so and nothing else breaks: code context
// becomes unavailable, never wrong.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::code {

struct RepositoryInfo {
  bool ok = false;
  std::string error;
  std::string root;
  std::string head;            // full SHA
  std::string branch;          // empty when detached
  std::string remote_url;      // origin, credentials stripped
  json::Value to_json() const;
};

struct CommitInfo {
  bool ok = false;
  std::string error;
  std::string sha;
  std::string author;          // name only: an e-mail is personal data
  std::string authored_at;     // ISO 8601
  std::string subject;
  std::vector<std::string> parents;
  json::Value to_json() const;
};

struct FileChange {
  std::string status;          // A M D R...
  std::string path;
  int added = -1, removed = -1;  // -1: binary or unknown
};

struct DiffSummary {
  bool ok = false;
  std::string error;
  std::string base, head;
  std::vector<FileChange> files;
  std::string patch;           // bounded
  bool patch_truncated = false;
  json::Value to_json() const;
};

struct SourceFile {
  bool ok = false;
  std::string error;
  std::string path, ref;
  std::string text;            // bounded
  bool truncated = false;
  json::Value to_json() const;
};

struct BlameInfo {
  bool ok = false;
  std::string error;
  std::string path;
  int line = 0;
  std::string commit;
  std::string author;          // name only
  std::string authored_at;
  std::string summary;
  std::string text;
  json::Value to_json() const;
};

class GitRepository {
 public:
  explicit GitRepository(std::string root);

  RepositoryInfo repository() const;
  CommitInfo commit(const std::string& ref) const;
  /// Files changed between two refs (head's parent when base is empty), with
  /// a patch limited to `max_patch_bytes` and to `paths` when given.
  DiffSummary diff(const std::string& base, const std::string& head,
                   std::size_t max_patch_bytes = 32 * 1024,
                   const std::vector<std::string>& paths = {}) const;
  SourceFile read_file(const std::string& path, const std::string& ref,
                       std::size_t max_bytes = 64 * 1024) const;
  /// `ref` empty: HEAD.
  BlameInfo blame(const std::string& path, int line, const std::string& ref) const;
  /// Lines [first, last] of a file at a ref, numbered.
  SourceFile lines(const std::string& path, const std::string& ref, int first, int last) const;
  /// A frame's file ("src/cache/VideoCache.ts", "/Users/ci/build/app/src/x.kt",
  /// "VideoCache.ts") to a tracked path, by the longest suffix that matches
  /// exactly one tracked file. Nullopt when none or several match: a guess
  /// would be worse than nothing.
  std::optional<std::string> resolve_path(const std::string& frame_file) const;
  /// Whether a commit exists here (a release built from another repository
  /// is not this one's).
  bool has_commit(const std::string& sha) const;

  const std::string& root() const { return root_; }

 private:
  std::string root_;
  mutable std::vector<std::string> tracked_;  // cached `git ls-files`
  mutable bool tracked_loaded_ = false;
};

/// A ref is a SHA, a branch or tag name: no leading '-', no "..", no
/// whitespace or control characters.
bool ref_is_safe(const std::string& ref);

}  // namespace mpi::code
