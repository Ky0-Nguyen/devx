// Session package on disk (spec section 14).
//
// manifest.json, capabilities.json, discovery.json, build.json,
// capture-config.json, raw/, issues.json, report.md, checksums.json.
//
// Raw traces are immutable, finalization is atomic, a partial session is
// recoverable and says it is partial, and deleting a session touches only
// files that session owns (spec D22, J08).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model/issue.hpp"
#include "core/model/trace.hpp"
#include "core/util/json.hpp"

namespace mpi::session {

// Capture lifecycle (spec section 6). Every transition is recorded so an
// interrupted session can be explained after the fact.
enum class SessionState {
  kIdle,
  kDiscovering,
  kPreflight,
  kReady,
  kRecording,
  kStopping,
  kProcessing,
  kCompleted,
  kCancelled,
  kFailed,
  kInterrupted,
  kPartial,
};
const char* to_string(SessionState s);

struct SessionManifest {
  std::string schema_version = "2.0";
  std::string session_id;
  std::string created_at;
  std::string finalized_at;
  SessionState state = SessionState::kIdle;
  std::string tool_version;
  model::MeasurementMode requested_mode = model::MeasurementMode::kDiagnostic;
  // Files in the package, relative to its root.
  std::vector<std::string> files;
  std::vector<std::string> state_transitions;
  bool synthetic = false;
  std::vector<std::string> partial_reasons;

  json::Value to_json() const;
};

struct WriteResult {
  bool ok = false;
  std::string error;
  std::string package_dir;
};

// Writes a complete session package. Content is written to a sibling temp
// directory and renamed into place, so an interrupted write never leaves a
// half-package that looks finished (spec section 14: atomic finalization).
// `extra_raw_files` are large artifacts copied into the package's `raw/`
// directory as {name, source path} -- a heap dump, say. They are checksummed
// with everything else, and a source that cannot be read fails the write
// rather than producing a package whose manifest lists a file that is not
// there.
WriteResult write_package(
    const std::string& parent_dir, const SessionManifest& manifest,
    const model::NormalizedTrace& trace, const model::AnalysisResult& analysis,
    const model::DiscoverySnapshot& discovery,
    const std::string& report_markdown, const std::string& report_json,
    const std::vector<std::pair<std::string, std::string>>& extra_raw_files = {});

struct LoadResult {
  bool ok = false;
  std::string error;
  SessionManifest manifest;
  // Checksum verification result per file. A failure is reported, not fatal:
  // the operator decides whether to trust the package.
  std::vector<std::string> checksum_failures;
  std::string trace_path;
  // A heap dump inside the package, when one was captured. Empty otherwise,
  // and empty means "this capture has none" -- not "it had none to find".
  std::string heap_path;
};

// Reads a package's manifest and verifies its checksums.
LoadResult load_package(const std::string& package_dir);

// FNV-1a 64 of a file's bytes, rendered as hex. Not a cryptographic digest:
// it detects corruption and accidental modification, which is what spec
// section 14's checksum validation is for. Stated plainly so nobody mistakes
// it for tamper protection.
std::optional<std::string> file_checksum(const std::string& path);

// Deletes a session package. Refuses any path that does not contain a
// manifest.json naming the expected session id, so a mistyped argument cannot
// delete an unrelated directory (spec J08).
struct DeleteResult {
  bool ok = false;
  std::string error;
  std::vector<std::string> removed;
};
DeleteResult delete_package(const std::string& package_dir,
                            const std::string& expected_session_id);

std::string new_session_id();

}  // namespace mpi::session
