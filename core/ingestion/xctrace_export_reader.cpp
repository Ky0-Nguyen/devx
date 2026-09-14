// Reads `xcrun xctrace export` output.
//
// This is the supported machine interface to an Instruments trace: the `.trace`
// bundle itself is an undocumented package, and Apple's own guidance is to
// export before consuming. Two shapes come out of it and this reader handles
// both, because a caller should not have to know which one they have:
//
//   --toc    ->  <trace-toc>          the run's device, target, window, template
//   --xpath  ->  <trace-query-result> the rows of one data table
//
// The format's one structural trick is a reference table. A value appears once
// with an `id`, and every later row points at it with `ref`:
//
//   <thread id="2" fmt="sh (0x3017f6)"><tid id="3">3151862</tid>...</thread>
//   ...
//   <thread ref="2"/>
//
// So the reader keeps what it has seen by id. A `ref` to an id that never
// appeared is a hole in the input, not something to fill in with a guess: the
// row is rejected and counted.
//
// What this reader will not do is decide it is looking at iOS. The TOC states
// its own platform, and a recording made against the host Mac says `macOS`.
// Carrying that through matters -- a macOS sample presented as iOS evidence
// would be a fabricated platform claim (spec J20).
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "core/ingestion/reader.hpp"
#include "core/util/xml.hpp"

namespace mpi::ingest {
namespace {

std::string slurp(const std::string& path, std::size_t max_bytes,
                  std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return {};
  }
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  if (size < 0) {
    error = "cannot size " + path;
    return {};
  }
  if (static_cast<std::size_t>(size) > max_bytes) {
    error = "input is " + std::to_string(size) + " bytes, over the limit of " +
            std::to_string(max_bytes);
    return {};
  }
  f.seekg(0, std::ios::beg);
  std::string data(static_cast<std::size_t>(size), '\0');
  f.read(data.data(), size);
  return data;
}

// The first bytes of the file, for sniffing without reading all of it.
std::string head_of(const std::string& path, std::size_t bytes) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::string buf(bytes, '\0');
  f.read(buf.data(), static_cast<std::streamsize>(bytes));
  buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

std::int64_t to_i64(const std::string& s) {
  std::int64_t v = 0;
  bool negative = false;
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    negative = s[i] == '-';
    ++i;
  }
  bool any = false;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') break;
    v = v * 10 + (s[i] - '0');
    any = true;
  }
  if (!any) return 0;
  return negative ? -v : v;
}

// One resolved column value. The format reuses ids across column kinds, so the
// table is keyed by id alone and each entry remembers what it was.
struct Referenced {
  std::string element;
  std::string text;
  // For a <thread> the tid and the owning pid/process live inside it.
  std::string tid;
  std::string pid;
  std::string process_name;
  std::vector<std::string> frames;
  // A `<binary>` carries the identity a symbol needs to be trusted: the UUID
  // is the build, and the path says whether the frame is app code or a
  // system library.
  std::string binary_name;
  std::string binary_uuid;
  std::string binary_path;
};

// --- the table of contents ---------------------------------------------------

bool read_toc(const std::string& data, model::NormalizedTrace& out,
              ReadDiagnostics& diag, const xml::Limits& limits) {
  xml::Parser p(data.data(), data.size(), limits);
  std::string platform;
  std::string device_model;
  std::string device_name;
  std::string os_version;
  std::string device_uuid;
  std::string process_name;
  std::string process_pid;
  std::string termination;
  std::string start_date;
  std::string end_date;
  std::string duration;
  std::string end_reason;
  std::string instruments_version;
  std::string template_name;
  std::string recording_mode;
  std::vector<std::string> schemas;
  std::string pending_text_for;
  // The recorded target is the `<process>` inside `<target>`. A TOC also
  // lists every process the trace observed -- the kernel included -- and
  // taking the last one seen pinned the target to `kernel`.
  bool in_target = false;
  std::vector<std::string> observed_processes;

  while (p.next()) {
    if (diag.cancelled) break;
    if (p.kind() == xml::NodeKind::kEndElement) {
      if (p.name() == "target") in_target = false;
      continue;
    }
    if (p.kind() == xml::NodeKind::kText) {
      if (pending_text_for == "start-date") start_date = p.text();
      if (pending_text_for == "end-date") end_date = p.text();
      if (pending_text_for == "duration") duration = p.text();
      if (pending_text_for == "end-reason") end_reason = p.text();
      if (pending_text_for == "instruments-version") instruments_version = p.text();
      if (pending_text_for == "template-name") template_name = p.text();
      if (pending_text_for == "recording-mode") recording_mode = p.text();
      pending_text_for.clear();
      continue;
    }
    if (p.kind() != xml::NodeKind::kStartElement) continue;

    if (p.name() == "target") {
      in_target = true;
      continue;
    }
    if (p.name() == "device") {
      platform = p.attribute("platform").value_or("");
      device_model = p.attribute("model").value_or("");
      device_name = p.attribute("name").value_or("");
      os_version = p.attribute("os-version").value_or("");
      device_uuid = p.attribute("uuid").value_or("");
    } else if (p.name() == "process") {
      if (in_target) {
        process_name = p.attribute("name").value_or("");
        process_pid = p.attribute("pid").value_or("");
        termination = p.attribute("termination-reason").value_or("");
      } else {
        // Every other process the trace saw. Recorded as context, never as
        // the target: a system-wide trace observes the kernel too.
        const auto name = p.attribute("name").value_or("");
        if (!name.empty()) observed_processes.push_back(name);
      }
    } else if (p.name() == "table") {
      if (const auto schema = p.attribute("schema")) schemas.push_back(*schema);
    } else {
      pending_text_for = p.name();
    }
  }
  if (p.failed()) {
    diag.errors.push_back("xctrace table of contents: " + p.error());
    return false;
  }

  // The platform is whatever the export said. A recording made against the
  // host Mac is macOS evidence and is labelled as such.
  if (platform == "iOS") {
    out.device.platform = model::Platform::kIos;
  } else if (platform == "macOS" || platform.empty()) {
    out.device.platform = model::Platform::kUnknown;
    diag.warnings.push_back(
        "this export records platform '" +
        (platform.empty() ? std::string("(unstated)") : platform) +
        "', not iOS. It is read as-is: a host recording is not evidence about "
        "an iOS app, and the platform is not overridden to make it look like "
        "one");
  } else {
    out.device.platform = model::Platform::kUnknown;
    diag.warnings.push_back("unrecognised export platform '" + platform + "'");
  }
  if (!device_uuid.empty()) out.device.device_id = device_uuid;
  out.device.display_name = device_name;
  out.device.model = device_model;
  out.device.os_version = os_version;
  // Instruments does not say whether it recorded a physical device or a
  // simulator, so the form stays unknown rather than being guessed. Keeping
  // simulator and physical results separate depends on knowing which (J18).
  out.device.form = model::DeviceForm::kUnknown;

  if (!process_name.empty()) {
    out.target.app.app_identifier = process_name;
    // An Instruments export carries no bundle identifier, so the kind
    // stays unknown rather than claiming a bundle id was observed.
    out.target.app.identifier_kind = model::IdentifierKind::kUnknown;
    out.target.app.platform = out.device.platform;
    out.target.app.device_id = out.device.device_id;
    diag.warnings.push_back(
        "the export identifies its target by process name ('" + process_name +
        "'), not by bundle id: an Instruments export carries no bundle "
        "identifier, so the target is pinned by name only");
  }
  if (!process_pid.empty()) {
    model::ProcessInstance proc;
    proc.app = out.target.app;
    proc.pid = static_cast<std::int32_t>(to_i64(process_pid));
    proc.process_name = process_name;
    proc.is_primary = true;
    // The recording tool attributed this process to the target it was asked
    // to record, which is provider attribution.
    proc.ownership = model::OwnershipEvidence::kProviderAttributed;
    proc.ownership_note = "the target of the xctrace recording";
    out.target.processes.push_back(std::move(proc));
  }

  const auto fact = [&](const char* key, const std::string& value,
                        const char* basis) {
    if (value.empty()) return;
    model::BuildFact f;
    f.key = key;
    f.value = value;
    f.source = model::FactSource::kHostToolchain;
    f.basis = basis;
    out.build.upsert(std::move(f));
  };
  fact("ios.instruments_version", instruments_version,
       "xctrace export table of contents");
  fact("ios.trace_template", template_name, "xctrace export table of contents");
  fact("ios.recording_mode", recording_mode, "xctrace export table of contents");
  fact("ios.export_platform", platform, "xctrace export table of contents");

  if (!termination.empty()) {
    out.ingestion_warnings.push_back("recorded process terminated: " +
                                     termination);
  }
  if (!end_reason.empty()) {
    out.ingestion_warnings.push_back("recording ended because: " + end_reason);
  }
  if (!schemas.empty()) {
    std::sort(schemas.begin(), schemas.end());
    schemas.erase(std::unique(schemas.begin(), schemas.end()), schemas.end());
    std::string list;
    for (const auto& s : schemas) {
      if (!list.empty()) list += ", ";
      list += s;
    }
    // The tables exist in the bundle; this reader turns only some of them
    // into evidence, and says so rather than implying the rest were empty.
    out.ingestion_warnings.push_back(
        "the trace holds " + std::to_string(schemas.size()) +
        " exportable table(s): " + list +
        ". A table this reader does not consume is unread, not empty");
  }

  if (!observed_processes.empty()) {
    std::sort(observed_processes.begin(), observed_processes.end());
    observed_processes.erase(
        std::unique(observed_processes.begin(), observed_processes.end()),
        observed_processes.end());
    out.ingestion_warnings.push_back(
        "the trace observed " + std::to_string(observed_processes.size()) +
        " process(es) besides the target; their activity is not this app's "
        "and is not attributed to it");
  }

  // A table of contents carries no samples. Saying so keeps a metadata-only
  // import from reading as a capture that found nothing.
  out.ingestion_warnings.push_back(
      "this is a table of contents only: it describes the run and contains no "
      "samples. Export a data table (for example schema=\"time-profile\") to "
      "get evidence");
  return true;
}

// --- one data table ----------------------------------------------------------

bool read_query_result(const std::string& data, const ReadOptions& opts,
                       model::NormalizedTrace& out, ReadDiagnostics& diag,
                       const xml::Limits& limits) {
  xml::Parser p(data.data(), data.size(), limits);
  std::map<std::string, Referenced> table;
  // name -> UUID, so the build identity behind the symbols is recorded
  // rather than thrown away with the parse.
  std::map<std::string, std::string> binaries;
  std::string schema;
  std::int64_t unresolved_refs = 0;

  // Row state, reset at every `<row>`.
  struct RowState {
    bool open = false;
    model::TimeNs sample_time = 0;
    bool have_time = false;
    std::string tid;
    std::string pid;
    std::string process_name;
    std::string thread_state;
    std::optional<double> weight;
    std::vector<std::string> frames;
  } row;

  // Which container element the nested leaves belong to. `<thread>` holds a
  // `<tid>` and a nested `<process>` holding a `<pid>`, so a leaf has to know
  // which of the two it is filling in.
  std::string open_thread_id;
  std::string open_process_id;
  std::string open_backtrace_id;
  // The leaf whose text is still to come, and the id to file it under.
  std::string pending_leaf;
  std::string pending_leaf_id;

  const auto remember = [&](const std::string& id, const Referenced& value) {
    if (!id.empty()) table[id] = value;
  };
  // The human label xctrace puts in `fmt`, e.g. `sh (99047)`, whose leading
  // token is the process name.
  const auto name_from_fmt = [](const std::string& fmt) {
    const auto paren = fmt.find(" (");
    return paren == std::string::npos ? fmt : fmt.substr(0, paren);
  };

  while (p.next()) {
    if (opts.cancel.cancelled()) {
      diag.cancelled = true;
      break;
    }

    if (p.kind() == xml::NodeKind::kText) {
      if (pending_leaf.empty()) continue;
      Referenced value;
      value.element = pending_leaf;
      value.text = p.text();

      if (pending_leaf == "sample-time") {
        row.sample_time = to_i64(p.text());
        row.have_time = true;
      } else if (pending_leaf == "tid") {
        row.tid = p.text();
        if (!open_thread_id.empty()) table[open_thread_id].tid = p.text();
      } else if (pending_leaf == "pid") {
        row.pid = p.text();
        if (!open_process_id.empty()) table[open_process_id].pid = p.text();
        // A pid inside a thread belongs to that thread too, so a later
        // `<thread ref>` still knows which process it ran in.
        if (!open_thread_id.empty()) table[open_thread_id].pid = p.text();
      } else if (pending_leaf == "thread-state") {
        row.thread_state = p.text();
      } else if (pending_leaf == "weight") {
        row.weight = static_cast<double>(to_i64(p.text()));
      }
      remember(pending_leaf_id, value);
      pending_leaf.clear();
      pending_leaf_id.clear();
      continue;
    }

    if (p.kind() == xml::NodeKind::kEndElement) {
      if (p.name() == "thread") {
        open_thread_id.clear();
      } else if (p.name() == "process") {
        open_process_id.clear();
      } else if (p.name() == "backtrace") {
        if (!open_backtrace_id.empty()) {
          Referenced value;
          value.element = "backtrace";
          value.frames = row.frames;
          remember(open_backtrace_id, value);
          open_backtrace_id.clear();
        }
      } else if (p.name() == "row" && row.open) {
        if (!row.have_time) {
          ++diag.events_rejected;
        } else {
          const std::string process_key =
              out.target.processes.empty()
                  ? (row.pid.empty() ? std::string() : "pid=" + row.pid)
                  : out.target.processes.front().canonical();

          model::CpuSample s;
          s.timestamp_ns = row.sample_time;
          s.provider =
              "xctrace " + (schema.empty() ? std::string("export") : schema);
          s.weight = row.weight;
          // xctrace lists the innermost frame first and the model is
          // outermost first, so the order is reversed rather than left
          // ambiguous (spec E15).
          s.frames.assign(row.frames.rbegin(), row.frames.rend());
          s.process_instance_id = process_key;
          if (!row.tid.empty()) s.thread_instance_id = "tid=" + row.tid;
          out.cpu_samples.push_back(std::move(s));
          ++diag.events_read;

          // A sample the provider marked as not running is not CPU time. It
          // is recorded as scheduling evidence so a blocked stretch stays
          // visible instead of vanishing.
          if (!row.thread_state.empty() && row.thread_state != "Running") {
            model::Event e;
            e.event_id = "xctrace-state-" + std::to_string(out.events.size());
            e.timestamp = row.sample_time;
            e.category = model::EventCategory::kSchedule;
            e.name = row.thread_state;
            e.provider = "xctrace " + schema;
            e.process_instance_id = process_key;
            e.thread_instance_id =
                row.tid.empty() ? std::string() : "tid=" + row.tid;
            out.events.push_back(std::move(e));
          }
        }
        row = RowState{};
      }
      continue;
    }

    if (p.kind() != xml::NodeKind::kStartElement) continue;

    if (p.name() == "schema") {
      schema = p.attribute("name").value_or("");
      continue;
    }
    if (p.name() == "row") {
      row = RowState{};
      row.open = true;
      continue;
    }
    if (!row.open) continue;

    // A back-reference: reuse the value this id was defined with.
    if (const auto ref = p.attribute("ref")) {
      const auto it = table.find(*ref);
      if (it == table.end()) {
        ++unresolved_refs;
        continue;
      }
      const Referenced& value = it->second;
      if (value.element == "sample-time") {
        row.sample_time = to_i64(value.text);
        row.have_time = true;
      } else if (value.element == "thread") {
        row.tid = value.tid;
        if (!value.pid.empty()) row.pid = value.pid;
        if (!value.process_name.empty()) row.process_name = value.process_name;
      } else if (value.element == "process") {
        if (!value.pid.empty()) row.pid = value.pid;
        if (!value.process_name.empty()) row.process_name = value.process_name;
      } else if (value.element == "thread-state") {
        row.thread_state = value.text;
      } else if (value.element == "weight") {
        row.weight = static_cast<double>(to_i64(value.text));
      } else if (value.element == "backtrace" ||
                 value.element == "tagged-backtrace") {
        row.frames = value.frames;
      } else if (value.element == "frame") {
        row.frames.push_back(value.text);
      } else if (value.element == "binary") {
        // Resolved and already accounted for: the binary belongs to the
        // frame that names it, which is recorded separately.
      }
      continue;
    }

    const std::string id = p.attribute("id").value_or("");
    if (p.name() == "frame") {
      // Frames repeat constantly across samples of the same stack, so after
      // the first appearance the export writes `<frame ref>`. They are
      // remembered by id for exactly that reason.
      const auto name = p.attribute("name");
      if (name) {
        row.frames.push_back(*name);
        Referenced value;
        value.element = "frame";
        value.text = *name;
        remember(id, value);
      }
      continue;
    }
    if (p.name() == "binary") {
      // Every frame names its binary, and after the first appearance the
      // export writes `<binary ref>`. Without remembering them, each of
      // those looked like a dangling reference: 184 of them in a 12-row
      // export.
      Referenced value;
      value.element = "binary";
      value.binary_name = p.attribute("name").value_or("");
      value.binary_uuid = p.attribute("UUID").value_or("");
      value.binary_path = p.attribute("path").value_or("");
      if (!value.binary_name.empty() && !value.binary_uuid.empty()) {
        binaries[value.binary_name] = value.binary_uuid;
      }
      remember(id, value);
      continue;
    }
    if (p.name() == "backtrace") {
      open_backtrace_id = id;
      row.frames.clear();
      continue;
    }
    if (p.name() == "tagged-backtrace") {
      // Rows reference either the tagged wrapper or the inner backtrace, so
      // the wrapper's id is filled in when the inner one closes.
      if (!id.empty()) {
        Referenced value;
        value.element = "tagged-backtrace";
        remember(id, value);
        open_backtrace_id = id;
      }
      continue;
    }
    if (p.name() == "thread" || p.name() == "process") {
      Referenced value;
      value.element = p.name();
      if (const auto fmt = p.attribute("fmt")) {
        value.process_name = name_from_fmt(*fmt);
        if (p.name() == "process") row.process_name = value.process_name;
      }
      remember(id, value);
      if (p.name() == "thread") {
        open_thread_id = id;
      } else {
        open_process_id = id;
      }
      continue;
    }
    // Anything else is a leaf whose value is its text.
    pending_leaf = p.name();
    pending_leaf_id = id;
  }

  if (p.failed()) {
    diag.errors.push_back("xctrace export: " + p.error());
    return false;
  }
  if (unresolved_refs > 0) {
    // A dangling reference is missing input. Counting it keeps the row count
    // honest instead of quietly shrinking the sample set.
    diag.warnings.push_back(
        std::to_string(unresolved_refs) +
        " column reference(s) pointed at an id this export never defined; "
        "those columns were left unset rather than guessed");
    out.dropped_events_by_collector["xctrace export"] += unresolved_refs;
  }
  for (const auto& [name, uuid] : binaries) {
    model::BuildFact f;
    f.key = "ios.binary_uuid." + name;
    f.value = uuid;
    f.source = model::FactSource::kHostToolchain;
    f.basis = "UUID attribute of a <binary> in the xctrace export";
    out.build.upsert(std::move(f));
  }
  if (!binaries.empty()) {
    out.ingestion_warnings.push_back(
        "the export named " + std::to_string(binaries.size()) +
        " binary image(s) with UUIDs; a symbol is only trustworthy against "
        "the build whose UUID matches");
  }

  if (!schema.empty()) {
    model::Capability c;
    c.id = "ios.capture.imported_table";
    c.human_name = "Imported xctrace table";
    c.status = out.cpu_samples.empty() ? model::CapabilityStatus::kLimited
                                       : model::CapabilityStatus::kAvailable;
    c.provider = "xctrace export";
    c.evidence = "schema '" + schema + "' yielded " +
                 std::to_string(out.cpu_samples.size()) + " sample(s)";
    c.tested = model::TestedState::kProbedOnly;
    c.limitations.push_back(
        "an import describes the run it came from; this tool did not observe "
        "the device and cannot vouch for how the recording was made");
    out.capabilities.upsert(std::move(c));
  }
  return !out.cpu_samples.empty() || !out.events.empty();
}

}  // namespace

// Declared in reader.hpp.
bool XctraceExportReader::can_read(const std::string& path) const {
  const auto head = head_of(path, 4096);
  if (head.empty()) return false;
  return contains(head, "<trace-toc") || contains(head, "<trace-query-result");
}

bool XctraceExportReader::read(const std::string& path,
                               const ReadOptions& opts,
                               model::NormalizedTrace& out,
                               ReadDiagnostics& diag) {
  xml::Limits limits;
  limits.max_bytes = opts.json_limits.max_bytes;

  std::string error;
  const std::string data = slurp(path, limits.max_bytes, error);
  if (!error.empty()) {
    diag.errors.push_back(error);
    return false;
  }
  if (opts.mark_synthetic) out.synthetic = true;

  if (contains(data.substr(0, 4096), "<trace-toc")) {
    return read_toc(data, out, diag, limits);
  }
  return read_query_result(data, opts, out, diag, limits);
}

}  // namespace mpi::ingest
