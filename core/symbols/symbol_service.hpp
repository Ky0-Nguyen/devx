// Symbol resolution and source attribution (spec section 11).
//
// The governing rule: never silently open the wrong revision or line. A
// symbol artifact is bound to an exact build identity or the resolution is
// reported as partial / mismatch / unavailable, and the UI refuses to
// navigate. Source paths are validated against traversal (spec J04) and never
// interpolated into a shell (spec J05, section 11).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model/build.hpp"
#include "core/model/issue.hpp"
#include "core/util/json.hpp"

namespace mpi::symbols {

// A JavaScript source map, loaded only when its bundle identity matches.
struct SourceMap {
  std::string path;
  // The bundle hash / OTA id this map belongs to. An empty value means the
  // map does not state one, which downgrades every resolution to "partial".
  std::string bundle_id;
  std::vector<std::string> sources;
  std::string source_root;
  // Decoded VLQ mappings: generated (line, column) -> source index, line, col.
  struct Mapping {
    int generated_line = 0;
    int generated_column = 0;
    int source_index = -1;
    int original_line = 0;
    int original_column = 0;
    int name_index = -1;
  };
  std::vector<Mapping> mappings;
  std::vector<std::string> names;

  // Nearest mapping at or before (line, column) on the same generated line.
  const Mapping* lookup(int generated_line, int generated_column) const;
};

// Loads a source map. Returns nullopt with `error` set on malformed input;
// size and nesting are bounded by the JSON limits.
std::optional<SourceMap> load_source_map(const std::string& path,
                                         const json::Limits& limits,
                                         std::string* error);

// Android R8/ProGuard mapping: obfuscated -> original class and member names.
struct ObfuscationMap {
  std::string path;
  // The build this map was produced for, when the file records one.
  std::string build_id;
  std::map<std::string, std::string> class_names;
  // "obfuscatedClass.obfuscatedMember" -> "originalClass.originalMember:line"
  std::map<std::string, std::string> members;

  std::optional<std::string> deobfuscate(const std::string& frame) const;
};

std::optional<ObfuscationMap> load_obfuscation_map(const std::string& path,
                                                   std::string* error);

// Rejects a path that escapes `root` via ".." or a symlink-style absolute
// jump, and any path containing a NUL. Spec J04, J19.
bool path_is_within_root(const std::string& root, const std::string& candidate);

// Maps a source path from the build machine's layout onto the local checkout.
// Monorepo remapping (spec G17) is a list of prefix rewrites applied in order.
struct SourceRootRemap {
  std::string from_prefix;
  std::string to_prefix;
};

class SymbolService {
 public:
  // Registers the artifacts available for this session together with the build
  // identity they must match.
  void set_expected_bundle_id(std::string id) { expected_bundle_id_ = std::move(id); }
  void set_expected_native_build_id(std::string id) {
    expected_native_build_id_ = std::move(id);
  }
  void add_source_map(SourceMap map);
  void add_obfuscation_map(ObfuscationMap map);
  void add_remap(SourceRootRemap r) { remaps_.push_back(std::move(r)); }
  void set_local_source_root(std::string root) { local_source_root_ = std::move(root); }
  // A dirty or mismatched checkout blocks exact source claims but not metric
  // inspection (spec section 7.2).
  void set_source_revision(std::string revision, bool dirty) {
    source_revision_ = std::move(revision);
    source_dirty_ = dirty;
  }

  // Resolves one raw provider frame. The returned location always carries a
  // symbol_status; `safe_to_open()` is true only on an exact build match with
  // a clean checkout and an existing local file.
  model::SourceLocation resolve_frame(const std::string& raw_frame) const;

  // The binding records that go into build.json, one per registered artifact.
  std::vector<model::SymbolBinding> bindings() const { return bindings_; }

  // Overall status across every artifact, for the issue-level symbol_status.
  std::string aggregate_status() const;

 private:
  std::string expected_bundle_id_;
  std::string expected_native_build_id_;
  std::vector<SourceMap> source_maps_;
  std::vector<ObfuscationMap> obfuscation_maps_;
  std::vector<SourceRootRemap> remaps_;
  std::string local_source_root_;
  std::string source_revision_;
  bool source_dirty_ = false;
  std::vector<model::SymbolBinding> bindings_;

  std::string apply_remaps(const std::string& path) const;
};

}  // namespace mpi::symbols
