// Ingestion contract.
//
// Every reader treats its input as untrusted (spec section 5), reports what it
// could not parse instead of dropping it silently, and yields a partial trace
// with explicit reasons rather than failing the whole session (spec D08, D19).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model/trace.hpp"
#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::ingest {

struct ReadOptions {
  json::Limits json_limits;
  CancellationToken cancel;
  // Only events belonging to these process instances are kept. Empty means
  // keep everything and mark ownership unknown.
  std::vector<std::string> keep_process_instance_ids;
  // Marks everything read as fixture-sourced (spec 0.6 / H15).
  bool mark_synthetic = false;
};

struct ReadDiagnostics {
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
  std::int64_t events_read = 0;
  std::int64_t events_rejected = 0;
  bool cancelled = false;
  bool truncated = false;

  bool fatal() const { return !errors.empty(); }
};

class Reader {
 public:
  virtual ~Reader() = default;
  virtual std::string id() const = 0;
  // Cheap sniff: can this reader plausibly handle the file?
  virtual bool can_read(const std::string& path) const = 0;
  // Reads into `out`. Returns false only when nothing usable was produced.
  virtual bool read(const std::string& path, const ReadOptions& opts,
                    model::NormalizedTrace& out, ReadDiagnostics& diag) = 0;
};

// Native session format: a full NormalizedTrace round-trip. Used by fixtures,
// session reopen, and any adapter that writes normalized output directly.
class MpiTraceReader final : public Reader {
 public:
  std::string id() const override { return "mpi.normalized.v2"; }
  bool can_read(const std::string& path) const override;
  bool read(const std::string& path, const ReadOptions& opts,
            model::NormalizedTrace& out, ReadDiagnostics& diag) override;
};

// Chrome / Perfetto JSON trace-event format ("traceEvents"). This is the
// format React Native's `react-native profile-hermes` output and several
// Android exports share, so it is the widest real on-ramp available without a
// device attached.
class ChromeTraceReader final : public Reader {
 public:
  std::string id() const override { return "chrome.trace_event.json"; }
  bool can_read(const std::string& path) const override;
  bool read(const std::string& path, const ReadOptions& opts,
            model::NormalizedTrace& out, ReadDiagnostics& diag) override;
};

// Hermes sampling profiler output (`stackFrames` + `samples` + `traceEvents`).
// Produces CPU samples on the JS clock domain; the mapping to the UI clock
// stays absent unless the capture supplied one.
class HermesProfileReader final : public Reader {
 public:
  std::string id() const override { return "hermes.sampling_profile"; }
  bool can_read(const std::string& path) const override;
  bool read(const std::string& path, const ReadOptions& opts,
            model::NormalizedTrace& out, ReadDiagnostics& diag) override;
};

// `xcrun xctrace export` output: either the run's table of contents
// (`--toc`) or the rows of one data table (`--xpath`). The `.trace` bundle
// itself is an undocumented package, so the export is the supported machine
// interface to an Instruments recording.
class XctraceExportReader final : public Reader {
 public:
  std::string id() const override { return "ios.xctrace.export"; }
  bool can_read(const std::string& path) const override;
  bool read(const std::string& path, const ReadOptions& opts,
            model::NormalizedTrace& out, ReadDiagnostics& diag) override;
};

// Picks a reader by sniffing, then reads. Returns the reader id used.
std::optional<std::string> read_any(const std::string& path,
                                    const ReadOptions& opts,
                                    model::NormalizedTrace& out,
                                    ReadDiagnostics& diag);

}  // namespace mpi::ingest
