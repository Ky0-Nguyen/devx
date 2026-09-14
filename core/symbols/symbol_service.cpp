#include "core/symbols/symbol_service.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mpi::symbols {
namespace {

// Base64 VLQ decoder for source-map "mappings". Rejects malformed input
// rather than producing an approximate offset.
int base64_char(char c) {
  static const std::string kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto pos = kAlphabet.find(c);
  return pos == std::string::npos ? -1 : static_cast<int>(pos);
}

bool decode_vlq(const std::string& s, std::size_t& i, int& out) {
  int result = 0;
  int shift = 0;
  bool any = false;
  for (;;) {
    if (i >= s.size()) return false;
    const int digit = base64_char(s[i]);
    if (digit < 0) return false;
    ++i;
    any = true;
    const bool has_continuation = (digit & 32) != 0;
    result += (digit & 31) << shift;
    shift += 5;
    if (!has_continuation) break;
    if (shift > 60) return false;  // malformed / overlong
  }
  if (!any) return false;
  const bool negative = (result & 1) != 0;
  result >>= 1;
  out = negative ? -result : result;
  return true;
}

bool file_exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Normalizes "a/./b/../c" without touching the filesystem, so a traversal
// attempt is rejected before any open() happens.
std::string lexically_normal(const std::string& in) {
  std::vector<std::string> parts;
  const bool absolute = !in.empty() && in.front() == '/';
  std::istringstream ss(in);
  std::string seg;
  while (std::getline(ss, seg, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!absolute) {
        parts.push_back("..");
      }
      continue;
    }
    parts.push_back(seg);
  }
  std::string out = absolute ? "/" : "";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += "/";
    out += parts[i];
  }
  return out.empty() ? "." : out;
}

}  // namespace

const SourceMap::Mapping* SourceMap::lookup(int generated_line,
                                            int generated_column) const {
  const Mapping* best = nullptr;
  for (const auto& m : mappings) {
    if (m.generated_line != generated_line) continue;
    if (m.generated_column > generated_column) continue;
    if (!best || m.generated_column > best->generated_column) best = &m;
  }
  return best;
}

std::optional<SourceMap> load_source_map(const std::string& path,
                                         const json::Limits& limits,
                                         std::string* error) {
  json::ParseError perr;
  auto parsed = json::parse_file(path, limits, &perr);
  if (!parsed) {
    if (error) *error = "source map parse failed: " + perr.message;
    return std::nullopt;
  }
  const json::Value& root = *parsed;
  const json::Value* version = root.find("version");
  if (!version || version->as_int() != 3) {
    if (error) {
      *error = "unsupported source map version (only v3 is supported)";
    }
    return std::nullopt;
  }
  SourceMap map;
  map.path = path;
  if (const json::Value* sr = root.find("sourceRoot"); sr && sr->is_string()) {
    map.source_root = sr->as_string();
  }
  // React Native / Metro records the bundle identity here when configured to.
  for (const char* key : {"x_react_native_bundle_id", "x_metro_bundle_id",
                          "x_facebook_sources_bundle_id", "file"}) {
    if (const json::Value* v = root.find(key); v && v->is_string() &&
                                               !v->as_string().empty()) {
      map.bundle_id = v->as_string();
      break;
    }
  }
  if (const json::Value* srcs = root.find("sources"); srcs && srcs->is_array()) {
    for (const auto& s : srcs->items()) {
      map.sources.push_back(s.is_string() ? s.as_string() : std::string());
    }
  }
  if (const json::Value* nms = root.find("names"); nms && nms->is_array()) {
    for (const auto& s : nms->items()) {
      map.names.push_back(s.is_string() ? s.as_string() : std::string());
    }
  }
  const json::Value* mappings = root.find("mappings");
  if (!mappings || !mappings->is_string()) {
    if (error) *error = "source map has no mappings string";
    return std::nullopt;
  }

  const std::string& enc = mappings->as_string();
  int gen_line = 0;
  int prev_source = 0;
  int prev_orig_line = 0;
  int prev_orig_col = 0;
  int prev_name = 0;
  int gen_col = 0;
  std::size_t i = 0;
  while (i < enc.size()) {
    const char c = enc[i];
    if (c == ';') {
      ++gen_line;
      gen_col = 0;
      ++i;
      continue;
    }
    if (c == ',') {
      ++i;
      continue;
    }
    int dcol = 0;
    if (!decode_vlq(enc, i, dcol)) {
      if (error) *error = "malformed VLQ in mappings";
      return std::nullopt;
    }
    gen_col += dcol;
    SourceMap::Mapping m;
    m.generated_line = gen_line;
    m.generated_column = gen_col;
    // A 1-field segment has no source information; record it as unmapped.
    if (i < enc.size() && enc[i] != ',' && enc[i] != ';') {
      int dsrc = 0, dline = 0, dcolo = 0;
      if (!decode_vlq(enc, i, dsrc) || !decode_vlq(enc, i, dline) ||
          !decode_vlq(enc, i, dcolo)) {
        if (error) *error = "truncated mapping segment";
        return std::nullopt;
      }
      prev_source += dsrc;
      prev_orig_line += dline;
      prev_orig_col += dcolo;
      m.source_index = prev_source;
      m.original_line = prev_orig_line;
      m.original_column = prev_orig_col;
      if (i < enc.size() && enc[i] != ',' && enc[i] != ';') {
        int dname = 0;
        if (!decode_vlq(enc, i, dname)) {
          if (error) *error = "truncated name index";
          return std::nullopt;
        }
        prev_name += dname;
        m.name_index = prev_name;
      }
    }
    map.mappings.push_back(m);
  }
  return map;
}

std::optional<std::string> ObfuscationMap::deobfuscate(
    const std::string& frame) const {
  // Frames arrive as "a.b.c(...)" or "a.b.c". Try the most specific
  // interpretation first: an exact member, then the whole string as a class,
  // and only then split the last segment off as a member of a class. Checking
  // the class before splitting matters: "a.b.c" is itself an obfuscated class
  // name, and splitting first would look up the non-existent class "a.b".
  const std::size_t paren = frame.find('(');
  const std::string bare = paren == std::string::npos ? frame : frame.substr(0, paren);

  if (auto member = members.find(bare); member != members.end()) {
    return member->second;
  }
  if (auto cls = class_names.find(bare); cls != class_names.end()) {
    return cls->second;
  }
  const std::size_t dot = bare.rfind('.');
  if (dot == std::string::npos) return std::nullopt;
  auto cls = class_names.find(bare.substr(0, dot));
  if (cls == class_names.end()) return std::nullopt;
  return cls->second + bare.substr(dot);
}

std::optional<ObfuscationMap> load_obfuscation_map(const std::string& path,
                                                   std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open obfuscation map: " + path;
    return std::nullopt;
  }
  ObfuscationMap map;
  map.path = path;
  std::string line;
  std::string current_obf_class;
  std::string current_orig_class;
  std::size_t bytes = 0;
  // R8 maps can be very large; cap the read the same way the JSON parser caps
  // its input (spec section 15).
  constexpr std::size_t kMaxBytes = 512ull * 1024ull * 1024ull;
  while (std::getline(f, line)) {
    bytes += line.size() + 1;
    if (bytes > kMaxBytes) {
      if (error) *error = "obfuscation map exceeds size limit";
      return std::nullopt;
    }
    if (line.empty()) continue;
    if (line[0] == '#') {
      // R8 header comments carry the build identity when present.
      const std::string marker = "# pg_map_id: ";
      if (line.rfind(marker, 0) == 0) map.build_id = line.substr(marker.size());
      continue;
    }
    if (line[0] != ' ' && line.find(" -> ") != std::string::npos) {
      const std::size_t arrow = line.find(" -> ");
      current_orig_class = line.substr(0, arrow);
      current_obf_class = line.substr(arrow + 4);
      if (!current_obf_class.empty() && current_obf_class.back() == ':') {
        current_obf_class.pop_back();
      }
      map.class_names[current_obf_class] = current_orig_class;
      continue;
    }
    // Member line: "    int foo() -> a" (optionally "12:34:int foo() -> a").
    const std::size_t arrow = line.find(" -> ");
    if (arrow == std::string::npos || current_obf_class.empty()) continue;
    std::string left = line.substr(0, arrow);
    const std::string right = line.substr(arrow + 4);
    // Strip leading whitespace and any "start:end:" line range.
    const std::size_t first = left.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    left = left.substr(first);
    std::string line_range;
    std::size_t colon_scan = 0;
    while (colon_scan < 2) {
      const std::size_t colon = left.find(':');
      if (colon == std::string::npos) break;
      const std::string head = left.substr(0, colon);
      if (head.find_first_not_of("0123456789") != std::string::npos) break;
      if (!line_range.empty()) line_range += ":";
      line_range += head;
      left = left.substr(colon + 1);
      ++colon_scan;
    }
    // left is now "int foo()"; take the name before '('.
    const std::size_t sp = left.find(' ');
    std::string signature = sp == std::string::npos ? left : left.substr(sp + 1);
    const std::size_t p = signature.find('(');
    const std::string name = p == std::string::npos ? signature : signature.substr(0, p);
    std::string value = current_orig_class + "." + name;
    if (!line_range.empty()) value += ":" + line_range;
    map.members[current_obf_class + "." + right] = value;
  }
  return map;
}

bool path_is_within_root(const std::string& root, const std::string& candidate) {
  if (candidate.find('\0') != std::string::npos) return false;
  if (root.empty()) return false;
  const std::string nroot = lexically_normal(root);
  // An absolute candidate is only acceptable if it already lies under root.
  const std::string joined =
      candidate.empty() ? nroot
      : candidate.front() == '/' ? lexically_normal(candidate)
                                 : lexically_normal(nroot + "/" + candidate);
  if (joined.size() < nroot.size()) return false;
  if (joined.compare(0, nroot.size(), nroot) != 0) return false;
  return joined.size() == nroot.size() || joined[nroot.size()] == '/';
}

void SymbolService::add_source_map(SourceMap map) {
  model::SymbolBinding b;
  b.kind = "js_source_map";
  b.artifact_path = map.path;
  b.expected_id = expected_bundle_id_;
  b.actual_id = map.bundle_id;
  if (expected_bundle_id_.empty()) {
    b.status = "partial";
    b.note =
        "no expected bundle identity was supplied, so the map cannot be proven "
        "to belong to the captured bundle";
  } else if (map.bundle_id.empty()) {
    b.status = "partial";
    b.note = "the source map does not record a bundle identity";
  } else if (map.bundle_id == expected_bundle_id_) {
    b.status = "exact_build_match";
  } else {
    b.status = "mismatch";
    b.note = "source map belongs to bundle '" + map.bundle_id +
             "' but the capture ran bundle '" + expected_bundle_id_ + "'";
  }
  bindings_.push_back(std::move(b));
  source_maps_.push_back(std::move(map));
}

void SymbolService::add_obfuscation_map(ObfuscationMap map) {
  model::SymbolBinding b;
  b.kind = "r8_map";
  b.artifact_path = map.path;
  b.expected_id = expected_native_build_id_;
  b.actual_id = map.build_id;
  if (expected_native_build_id_.empty() || map.build_id.empty()) {
    b.status = "partial";
    b.note =
        "the mapping file could not be bound to an exact build id; names may "
        "come from a different build";
  } else if (map.build_id == expected_native_build_id_) {
    b.status = "exact_build_match";
  } else {
    b.status = "mismatch";
    b.note = "mapping build id '" + map.build_id + "' != capture build id '" +
             expected_native_build_id_ + "'";
  }
  bindings_.push_back(std::move(b));
  obfuscation_maps_.push_back(std::move(map));
}

std::string SymbolService::apply_remaps(const std::string& path) const {
  std::string out = path;
  for (const auto& r : remaps_) {
    if (r.from_prefix.empty()) continue;
    if (out.rfind(r.from_prefix, 0) == 0) {
      out = r.to_prefix + out.substr(r.from_prefix.size());
      break;  // first matching remap wins, in declaration order
    }
  }
  return out;
}

model::SourceLocation SymbolService::resolve_frame(
    const std::string& raw_frame) const {
  model::SourceLocation loc;
  loc.symbol = raw_frame;

  // Android obfuscated frame.
  for (const auto& om : obfuscation_maps_) {
    if (auto d = om.deobfuscate(raw_frame)) {
      loc.symbol = *d;
      // Find this artifact's binding status.
      for (const auto& b : bindings_) {
        if (b.artifact_path == om.path) {
          loc.symbol_status = b.status == "exact_build_match" ? "exact_build_match"
                                                             : b.status;
          loc.note = b.note;
          break;
        }
      }
      break;
    }
  }

  // A Hermes frame of the form "name(address:line:column)" or
  // "name (bundle.js:1234:56)" carries a generated position we can map.
  const std::size_t open = raw_frame.rfind('(');
  const std::size_t close = raw_frame.rfind(')');
  if (open != std::string::npos && close != std::string::npos && close > open) {
    const std::string inner = raw_frame.substr(open + 1, close - open - 1);
    const std::size_t c2 = inner.rfind(':');
    const std::size_t c1 = c2 == std::string::npos ? std::string::npos
                                                   : inner.rfind(':', c2 - 1);
    if (c1 != std::string::npos && c2 != std::string::npos) {
      const std::string line_s = inner.substr(c1 + 1, c2 - c1 - 1);
      const std::string col_s = inner.substr(c2 + 1);
      if (!line_s.empty() &&
          line_s.find_first_not_of("0123456789") == std::string::npos &&
          !col_s.empty() &&
          col_s.find_first_not_of("0123456789") == std::string::npos) {
        const int gen_line = std::atoi(line_s.c_str()) - 1;  // maps are 0-based
        const int gen_col = std::atoi(col_s.c_str());
        for (std::size_t mi = 0; mi < source_maps_.size(); ++mi) {
          const SourceMap& sm = source_maps_[mi];
          const SourceMap::Mapping* m = sm.lookup(gen_line, gen_col);
          if (!m || m->source_index < 0 ||
              static_cast<std::size_t>(m->source_index) >= sm.sources.size()) {
            continue;
          }
          std::string src = sm.sources[static_cast<std::size_t>(m->source_index)];
          if (!sm.source_root.empty() && !src.empty() && src.front() != '/') {
            src = sm.source_root + "/" + src;
          }
          loc.file = apply_remaps(src);
          loc.line = m->original_line + 1;
          loc.column = m->original_column;
          if (m->name_index >= 0 &&
              static_cast<std::size_t>(m->name_index) < sm.names.size()) {
            loc.symbol = sm.names[static_cast<std::size_t>(m->name_index)];
          }
          // Status comes from the artifact binding, never from the fact that
          // a lookup happened to succeed.
          for (const auto& b : bindings_) {
            if (b.artifact_path == sm.path) {
              loc.symbol_status = b.status;
              loc.note = b.note;
              break;
            }
          }
          break;
        }
      }
    }
  }

  if (loc.symbol_status == "exact_build_match") {
    // Three further conditions must hold before the UI may open the file.
    if (source_dirty_) {
      loc.symbol_status = "partial";
      loc.note = "local checkout is dirty; the line may not match the build";
    } else if (loc.file.empty()) {
      loc.symbol_status = "partial";
      loc.note = "symbol resolved but no source file is associated";
    } else if (local_source_root_.empty()) {
      loc.symbol_status = "partial";
      loc.note = "no local source root configured";
    } else if (!path_is_within_root(local_source_root_, loc.file)) {
      // Spec J04 / G19: a map may name any path; we refuse to leave the root.
      loc.symbol_status = "mismatch";
      loc.note = "source path resolves outside the configured source root and "
                 "was rejected";
    } else {
      const std::string abs = loc.file.front() == '/'
                                  ? loc.file
                                  : local_source_root_ + "/" + loc.file;
      if (!file_exists(abs)) {
        loc.symbol_status = "partial";
        loc.note = "source file not present locally: " + loc.file;
      }
    }
  } else if (loc.symbol_status == "unavailable" && loc.file.empty()) {
    loc.note = source_maps_.empty() && obfuscation_maps_.empty()
                   ? "no symbol artifacts were supplied for this session"
                   : "no supplied artifact resolved this frame";
  }
  return loc;
}

std::string SymbolService::aggregate_status() const {
  if (bindings_.empty()) return "unavailable";
  bool any_exact = false;
  bool any_mismatch = false;
  bool any_partial = false;
  for (const auto& b : bindings_) {
    if (b.status == "exact_build_match") any_exact = true;
    else if (b.status == "mismatch") any_mismatch = true;
    else if (b.status == "partial") any_partial = true;
  }
  // A mismatch is reported even when something else matched: it is the fact
  // that would make a source claim wrong.
  if (any_mismatch) return "mismatch";
  if (any_exact && !any_partial) return "exact_build_match";
  if (any_exact || any_partial) return "partial";
  return "unresolved";
}

}  // namespace mpi::symbols
