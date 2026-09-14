// `mpi timeline` -- the binned timeline, as JSON or as text.
//
// The text renderer is not a convenience. A chart's failure mode is that an
// unmeasured stretch and a measured zero look identical, and the fastest way
// to check that the data keeps them apart is to draw it in a terminal where
// the two use different glyphs. The legend is printed with every render so a
// blank column can never be read as a quiet one.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

#include "apps/cli/cli.hpp"
#include "core/ingestion/normalize.hpp"
#include "core/ingestion/reader.hpp"
#include "core/rules/engine.hpp"
#include "core/session/session_store.hpp"
#include "core/symbols/symbol_service.hpp"
#include "core/timeline/timeline.hpp"

namespace mpi::cli {
namespace {

bool is_session_dir(const std::string& path) {
  std::ifstream f(path + "/manifest.json");
  return f.good();
}

// Eight levels for a measured bin, and two glyphs that are not levels at all:
// ' ' never appears for a measured value, so an empty column always means
// "not measured", and '?' marks a bin whose number is incomplete.
char glyph_for(const timeline::Bin& bin, double scale) {
  switch (bin.state) {
    case timeline::BinState::kUnmeasured:
      return ' ';
    case timeline::BinState::kNoReading:
      // The collector was running and this quantity was not sampled here.
      // Distinct from both a blank (nobody looked) and a '.' (looked, and the
      // answer was zero).
      return '-';
    case timeline::BinState::kPartial:
      return '?';
    case timeline::BinState::kMeasured:
      break;
  }
  const double v = bin.value.value_or(0.0);
  if (v <= 0.0) return '.';  // a measured zero: a mark, not a blank
  if (scale <= 0.0) return '#';
  static const char* kLevels = "▁▂▃▄▅▆▇█";
  const double frac = std::clamp(v / scale, 0.0, 1.0);
  auto idx = static_cast<std::size_t>(std::lround(frac * 7.0));
  // Multi-byte: the glyphs are three bytes each, so this returns the lead
  // byte's index instead. Handled by the caller, which appends the whole
  // code point.
  static_cast<void>(kLevels);
  return static_cast<char>('0' + idx);
}

std::string bar_for(const timeline::Track& track) {
  static const char* kLevels[] = {"▁", "▂", "▃", "▄",
                                  "▅", "▆", "▇", "█"};
  const double scale = track.max_value.value_or(0.0);
  std::string out;
  for (const auto& bin : track.bins) {
    const char c = glyph_for(bin, scale);
    if (c >= '0' && c <= '7') {
      out += kLevels[static_cast<std::size_t>(c - '0')];
    } else {
      out += c;
    }
  }
  return out;
}

std::string human_ns(model::TimeNs ns) {
  // Signed on purpose. A gap that starts before the window is a real thing a
  // capture can contain, and rendering it as an unsigned microsecond count
  // printed "+-5209399688 us", which looks like a formatting accident rather
  // than the fact it is.
  const char* sign = ns < 0 ? "-" : "";
  const double abs_ns = std::fabs(static_cast<double>(ns));
  const double ms = abs_ns / 1e6;
  char buf[64];
  if (ms < 1.0) {
    std::snprintf(buf, sizeof(buf), "%s%.0f us", sign, abs_ns / 1e3);
  } else if (ms < 1000.0) {
    std::snprintf(buf, sizeof(buf), "%s%.1f ms", sign, ms);
  } else {
    std::snprintf(buf, sizeof(buf), "%s%.2f s", sign, ms / 1000.0);
  }
  return buf;
}

// A band's position relative to the window, said in a way that survives a
// band lying outside it.
std::string offset_of(model::TimeNs at, model::TimeNs window_start,
                      model::TimeNs window_end) {
  if (at < window_start) {
    return human_ns(at - window_start) + " (before the window began)";
  }
  if (at > window_end) {
    return "+" + human_ns(at - window_start) + " (after the window ended)";
  }
  return "+" + human_ns(at - window_start);
}

void render_text(const timeline::Timeline& tl) {
  if (!tl.empty_reason.empty()) {
    std::cout << "no timeline: " << tl.empty_reason << "\n";
    return;
  }
  std::cout << "session   " << tl.session_id << "\n"
            << "window    " << human_ns(tl.window_end_ns - tl.window_start_ns)
            << " in " << tl.bin_count << " bins of "
            << human_ns(tl.bin_width_ns) << "\n"
            << "clock     " << tl.primary_clock_domain << "\n\n";

  // The legend comes before the tracks on purpose: the reader meets the
  // meaning of a blank column before meeting a blank column.
  std::cout << "legend    " << "▁..█ measured value   "
            << ". measured zero   "
            << "- covered, not sampled here   "
            << "? partly covered (value is wrong-low)   "
            << "(blank) NOT MEASURED\n\n";

  std::size_t width = 0;
  for (const auto& t : tl.tracks) width = std::max(width, t.label.size());

  for (const auto& t : tl.tracks) {
    std::string label = t.label;
    label.resize(std::max(width, std::size_t{12}), ' ');
    if (!t.placed) {
      std::cout << label << "  [not placed] " << t.unplaced_events
                << " event(s)\n"
                << std::string(width + 2, ' ') << "  " << t.placement_note
                << "\n";
      continue;
    }
    std::cout << label << "  " << bar_for(t);
    if (t.max_value.has_value()) {
      std::cout << "  peak " << *t.max_value << " " << t.unit;
    } else {
      std::cout << "  no measured bin";
    }
    std::cout << "\n";
    // The basis for every blank column above, printed whether or not there
    // are any: "nobody looked" and "the collector looked and saw nothing" are
    // the two readings this line separates.
    if (!t.coverage_note.empty()) {
      std::cout << std::string(width + 2, ' ') << "  " << t.coverage_note
                << "\n";
    }
    if (t.unplaced_events > 0) {
      std::cout << std::string(width + 2, ' ') << "  " << t.unplaced_events
                << " event(s) outside the window, not drawn\n";
    }
  }

  if (!tl.gaps.empty()) {
    std::cout << "\ncoverage gaps (from the collectors' own records):\n";
    for (const auto& g : tl.gaps) {
      std::cout << "  " << g.label << "  "
                << offset_of(g.start_ns, tl.window_start_ns, tl.window_end_ns)
                << " for " << human_ns(g.end_ns - g.start_ns) << "  "
                << g.detail << "\n";
    }
  }
  if (!tl.issues.empty()) {
    std::cout << "\nissue intervals (exact, for focusing):\n";
    for (const auto& i : tl.issues) {
      std::cout << "  " << i.label << "  "
                << offset_of(i.start_ns, tl.window_start_ns, tl.window_end_ns)
                << " for "
                << human_ns(i.end_ns - i.start_ns)
                << (i.widened ? "  [widened to be visible; the evidence is an "
                                "instant]" : "")
                << "  " << i.detail << "\n";
    }
  }
  // Limitations last and unconditionally: they are what the picture above
  // does not say.
  std::cout << "\nwhat these tracks do not say:\n";
  for (const auto& t : tl.tracks) {
    for (const auto& l : t.limitations) {
      std::cout << "  " << t.label << ": " << l << "\n";
    }
  }
}

}  // namespace

ExitCode cmd_timeline(const Invocation& inv) {
  if (inv.positional.empty()) {
    std::cerr << "error: timeline needs a session directory or a trace file\n";
    return ExitCode::kUsage;
  }
  const std::string input = inv.positional.front();

  int bins = 120;
  if (inv.has_flag("bins")) {
    bins = std::atoi(inv.flag("bins").c_str());
    if (bins < 1 || bins > 4096) {
      std::cerr << "error: --bins must be between 1 and 4096\n";
      return ExitCode::kUsage;
    }
  }

  std::string trace_path = input;
  std::vector<std::string> notes;
  if (is_session_dir(input)) {
    const auto loaded = session::load_package(input);
    if (!loaded.ok) {
      std::cerr << "error: " << loaded.error << "\n";
      return ExitCode::kCollectionError;
    }
    trace_path = loaded.trace_path;
    for (const auto& f : loaded.checksum_failures) {
      warn("session package checksum problem: " + f);
      notes.push_back("checksum: " + f);
    }
  }

  ingest::ReadOptions read_opts;
  read_opts.cancel = inv.global.cancel;
  if (inv.has_flag("max-input-mib")) {
    const long long mib = std::atoll(inv.flag("max-input-mib").c_str());
    if (mib <= 0) {
      std::cerr << "error: --max-input-mib must be a positive integer\n";
      return ExitCode::kUsage;
    }
    read_opts.json_limits.max_bytes =
        static_cast<std::size_t>(mib) * 1024ull * 1024ull;
  }
  model::NormalizedTrace trace;
  ingest::ReadDiagnostics diag;
  const auto reader_id = ingest::read_any(trace_path, read_opts, trace, diag);
  if (!reader_id.has_value()) {
    for (const auto& e : diag.errors) std::cerr << "error: " << e << "\n";
    return diag.errors.empty() ? ExitCode::kUnsupportedOperation
                               : ExitCode::kCollectionError;
  }
  ingest::normalize(trace, inv.global.cancel);

  // The issue bands come from running the engine over the same trace, so a
  // band can never point at an interval the current ruleset did not produce.
  symbols::SymbolService symbols;
  rules::EngineOptions engine_opts;
  engine_opts.cancel = inv.global.cancel;
  engine_opts.mode = model::measurement_mode_from_string(
      inv.flag("mode", "diagnostic"));
  const auto analysis = rules::analyze(trace, symbols, engine_opts);

  timeline::Options opts;
  opts.bin_count = bins;
  opts.cancel = inv.global.cancel;
  // A band has to be clickable, and half a bin is the smallest width that is
  // still unambiguous about which bin it belongs to.
  if (trace.window_end_ns > trace.window_start_ns) {
    opts.min_band_ns = (trace.window_end_ns - trace.window_start_ns) /
                       (static_cast<model::TimeNs>(bins) * 2);
  }
  const auto tl = timeline::build(trace, analysis.issues, opts);

  if (inv.global.json) {
    json::Value out = tl.to_json();
    json::Value n = json::Value::array();
    for (const auto& note : notes) n.push_back(json::Value::string(note));
    out.set("notes", std::move(n));
    out.set("synthetic", json::Value::boolean(trace.synthetic));
    out.set("partial", json::Value::boolean(trace.partial));
    print_json(out);
  } else {
    if (trace.synthetic) {
      std::cout << "SYNTHETIC: this capture came from a labelled fixture and "
                   "describes no real application.\n\n";
    }
    if (trace.partial) {
      std::cout << "PARTIAL: the recording ended abnormally, so an empty "
                   "stretch may be the recording stopping.\n\n";
    }
    render_text(tl);
  }
  return ExitCode::kOk;
}

}  // namespace mpi::cli
