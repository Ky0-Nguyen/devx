// Observations kept on disk, so an AI tool can read them later.
//
// A session package holds a capture: measurements, analysed. Other things this
// tool produces are not measurements -- a layout snapshot, an inspect
// observation of the app's network, console and Redux -- and used to exist only
// on screen or on stdout. Kept here, `mpi mcp` can list and read them, and a
// model can work from the whole document rather than from a screenshot of it.
//
// One JSON file per observation under `<sessions dir>/observations/`, in an
// envelope that says what it is, when it was saved, and for which app. The
// directory is owner-only and so is every file: an inspect observation taken
// with `--detail` holds request headers, bearer tokens included, verbatim.
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "core/util/json.hpp"

namespace mpi::observe {

std::string observations_dir(const std::string& sessions_dir);

struct SavedObservation {
  bool ok = false;
  std::string id;
  std::string path;
  std::string error;
};

/// Writes `document` atomically (temporary file, then rename). `kind` is a
/// short word -- "layout", "inspect" -- and becomes part of the id. `summary`
/// is a small object a listing can show without the whole document.
SavedObservation save_observation(const std::string& sessions_dir,
                                  const std::string& kind,
                                  const std::string& app_identifier,
                                  const std::string& device_id,
                                  const json::Value& summary,
                                  const json::Value& document);

/// The newest first, at most `limit`, optionally only one kind. Each entry is
/// the envelope without its document.
json::Value list_observations(const std::string& sessions_dir,
                              const std::string& kind, std::size_t limit);

/// One observation, envelope and document. Refuses an id that could name a
/// file outside the directory.
std::optional<json::Value> read_observation(const std::string& sessions_dir,
                                            const std::string& id,
                                            std::string* error);

bool observation_id_is_safe(const std::string& id);

/// The listing summary for an inspect observation, from its JSON: how many
/// network exchanges, console lines and Redux records, whether a debugger
/// attached, and whether headers or bodies were captured -- the last because
/// that is the file holding credentials.
json::Value summarize_inspect(const json::Value& report);

}  // namespace mpi::observe
