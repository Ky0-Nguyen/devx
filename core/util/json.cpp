#include "core/util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace mpi::json {
namespace {

const std::string kEmptyString;

struct Parser {
  std::string_view in;
  std::size_t pos = 0;
  const Limits& limits;
  ParseError* err;

  bool fail(const char* msg) {
    if (err) {
      err->message = msg;
      err->offset = pos;
      std::size_t line = 1;
      for (std::size_t i = 0; i < pos && i < in.size(); ++i) {
        if (in[i] == '\n') ++line;
      }
      err->line = line;
    }
    return false;
  }

  void skip_ws() {
    while (pos < in.size()) {
      const char c = in[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
      } else {
        break;
      }
    }
  }

  bool parse_value(Value& out, std::size_t depth) {
    if (depth > limits.max_depth) return fail("max nesting depth exceeded");
    skip_ws();
    if (pos >= in.size()) return fail("unexpected end of input");
    switch (in[pos]) {
      case 'n':
        return literal("null", Value::null(), out);
      case 't':
        return literal("true", Value::boolean(true), out);
      case 'f':
        return literal("false", Value::boolean(false), out);
      case '"': {
        std::string s;
        if (!parse_string(s)) return false;
        out = Value::string(std::move(s));
        return true;
      }
      case '[':
        return parse_array(out, depth);
      case '{':
        return parse_object(out, depth);
      default:
        return parse_number(out);
    }
  }

  bool literal(const char* lit, Value v, Value& out) {
    const std::size_t n = std::strlen(lit);
    if (in.size() - pos < n || in.compare(pos, n, lit) != 0) {
      return fail("invalid literal");
    }
    pos += n;
    out = std::move(v);
    return true;
  }

  bool append_utf8(std::string& s, std::uint32_t cp) {
    if (cp <= 0x7F) {
      s.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      return false;
    }
    return true;
  }

  bool parse_hex4(std::uint32_t& out) {
    if (in.size() - pos < 4) return fail("truncated \\u escape");
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = in[pos + static_cast<std::size_t>(i)];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    pos += 4;
    out = v;
    return true;
  }

  bool parse_string(std::string& s) {
    if (pos >= in.size() || in[pos] != '"') return fail("expected string");
    ++pos;
    for (;;) {
      if (pos >= in.size()) return fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(in[pos]);
      if (c == '"') {
        ++pos;
        return true;
      }
      if (s.size() > limits.max_string_bytes) {
        return fail("string exceeds max_string_bytes");
      }
      if (c == '\\') {
        ++pos;
        if (pos >= in.size()) return fail("unterminated escape");
        const char e = in[pos++];
        switch (e) {
          case '"': s.push_back('"'); break;
          case '\\': s.push_back('\\'); break;
          case '/': s.push_back('/'); break;
          case 'b': s.push_back('\b'); break;
          case 'f': s.push_back('\f'); break;
          case 'n': s.push_back('\n'); break;
          case 'r': s.push_back('\r'); break;
          case 't': s.push_back('\t'); break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!parse_hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // High surrogate: a low surrogate must follow.
              if (in.size() - pos >= 2 && in[pos] == '\\' && in[pos + 1] == 'u') {
                pos += 2;
                std::uint32_t lo = 0;
                if (!parse_hex4(lo)) return false;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                  // Unpaired: emit both as replacement rather than guessing.
                  if (!append_utf8(s, 0xFFFD)) return fail("bad code point");
                  cp = 0xFFFD;
                }
              } else {
                cp = 0xFFFD;
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              cp = 0xFFFD;  // lone low surrogate
            }
            if (!append_utf8(s, cp)) return fail("bad code point");
            break;
          }
          default:
            return fail("invalid escape sequence");
        }
        continue;
      }
      if (c < 0x20) return fail("unescaped control character in string");
      s.push_back(static_cast<char>(c));
      ++pos;
    }
  }

  bool parse_number(Value& out) {
    const std::size_t start = pos;
    if (pos < in.size() && (in[pos] == '-' || in[pos] == '+')) ++pos;
    bool any_digit = false;
    while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') {
      ++pos;
      any_digit = true;
    }
    bool is_float = false;
    if (pos < in.size() && in[pos] == '.') {
      is_float = true;
      ++pos;
      while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') {
        ++pos;
        any_digit = true;
      }
    }
    if (pos < in.size() && (in[pos] == 'e' || in[pos] == 'E')) {
      is_float = true;
      ++pos;
      if (pos < in.size() && (in[pos] == '-' || in[pos] == '+')) ++pos;
      bool exp_digit = false;
      while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') {
        ++pos;
        exp_digit = true;
      }
      if (!exp_digit) return fail("malformed exponent");
    }
    if (!any_digit) return fail("expected value");
    const std::string text(in.substr(start, pos - start));
    if (!is_float) {
      errno = 0;
      char* end = nullptr;
      const long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == 0 && end && *end == '\0') {
        out = Value::integer(static_cast<std::int64_t>(v));
        return true;
      }
      // Integer overflow: keep the magnitude as a double rather than wrapping.
    }
    errno = 0;
    char* end = nullptr;
    const double d = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0') return fail("malformed number");
    out = Value::number(d);
    return true;
  }

  bool parse_array(Value& out, std::size_t depth) {
    ++pos;  // '['
    std::vector<Value> items;
    skip_ws();
    if (pos < in.size() && in[pos] == ']') {
      ++pos;
      out = Value::array(std::move(items));
      return true;
    }
    for (;;) {
      if (items.size() >= limits.max_container_elements) {
        return fail("array exceeds max_container_elements");
      }
      Value v;
      if (!parse_value(v, depth + 1)) return false;
      items.push_back(std::move(v));
      skip_ws();
      if (pos >= in.size()) return fail("unterminated array");
      if (in[pos] == ',') {
        ++pos;
        continue;
      }
      if (in[pos] == ']') {
        ++pos;
        out = Value::array(std::move(items));
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  // Walks a value without materialising it. Used by the streaming parser to
  // discard members a caller does not want, and to skip past array elements.
  bool skip_value(std::size_t depth) {
    if (depth > limits.max_depth) return fail("max nesting depth exceeded");
    skip_ws();
    if (pos >= in.size()) return fail("unexpected end of input");
    switch (in[pos]) {
      case 'n': {
        Value ignored;
        return literal("null", Value::null(), ignored);
      }
      case 't': {
        Value ignored;
        return literal("true", Value::boolean(true), ignored);
      }
      case 'f': {
        Value ignored;
        return literal("false", Value::boolean(false), ignored);
      }
      case '"': {
        std::string ignored;
        return parse_string(ignored);
      }
      case '[': {
        ++pos;
        skip_ws();
        if (pos < in.size() && in[pos] == ']') {
          ++pos;
          return true;
        }
        for (;;) {
          if (!skip_value(depth + 1)) return false;
          skip_ws();
          if (pos >= in.size()) return fail("unterminated array");
          if (in[pos] == ',') {
            ++pos;
            continue;
          }
          if (in[pos] == ']') {
            ++pos;
            return true;
          }
          return fail("expected ',' or ']'");
        }
      }
      case '{': {
        ++pos;
        skip_ws();
        if (pos < in.size() && in[pos] == '}') {
          ++pos;
          return true;
        }
        for (;;) {
          skip_ws();
          std::string key;
          if (!parse_string(key)) return false;
          skip_ws();
          if (pos >= in.size() || in[pos] != ':') return fail("expected ':'");
          ++pos;
          if (!skip_value(depth + 1)) return false;
          skip_ws();
          if (pos >= in.size()) return fail("unterminated object");
          if (in[pos] == ',') {
            ++pos;
            continue;
          }
          if (in[pos] == '}') {
            ++pos;
            return true;
          }
          return fail("expected ',' or '}'");
        }
      }
      default: {
        Value ignored;
        return parse_number(ignored);
      }
    }
  }

  bool parse_object(Value& out, std::size_t depth) {
    ++pos;  // '{'
    std::vector<Member> members;
    skip_ws();
    if (pos < in.size() && in[pos] == '}') {
      ++pos;
      out = Value::object(std::move(members));
      return true;
    }
    for (;;) {
      if (members.size() >= limits.max_container_elements) {
        return fail("object exceeds max_container_elements");
      }
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos >= in.size() || in[pos] != ':') return fail("expected ':'");
      ++pos;
      Value v;
      if (!parse_value(v, depth + 1)) return false;
      members.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (pos >= in.size()) return fail("unterminated object");
      if (in[pos] == ',') {
        ++pos;
        continue;
      }
      if (in[pos] == '}') {
        ++pos;
        out = Value::object(std::move(members));
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }
};

void write_indent(std::string& out, int indent, int level) {
  if (indent < 0) return;
  out.push_back('\n');
  out.append(static_cast<std::size_t>(indent * level), ' ');
}

}  // namespace

Value Value::boolean(bool v) {
  Value x;
  x.type_ = Type::Bool;
  x.b_ = v;
  return x;
}
Value Value::integer(std::int64_t v) {
  Value x;
  x.type_ = Type::Int;
  x.i_ = v;
  return x;
}
Value Value::number(double v) {
  Value x;
  x.type_ = Type::Double;
  x.d_ = v;
  return x;
}
Value Value::string(std::string v) {
  Value x;
  x.type_ = Type::String;
  x.s_ = std::move(v);
  return x;
}
Value Value::array(std::vector<Value> v) {
  Value x;
  x.type_ = Type::Array;
  x.arr_ = std::move(v);
  return x;
}
Value Value::object(std::vector<Member> v) {
  Value x;
  x.type_ = Type::Object;
  x.obj_ = std::move(v);
  return x;
}

bool Value::as_bool(bool fallback) const {
  return type_ == Type::Bool ? b_ : fallback;
}
std::int64_t Value::as_int(std::int64_t fallback) const {
  if (type_ == Type::Int) return i_;
  if (type_ == Type::Double) return static_cast<std::int64_t>(d_);
  return fallback;
}
double Value::as_double(double fallback) const {
  if (type_ == Type::Double) return d_;
  if (type_ == Type::Int) return static_cast<double>(i_);
  return fallback;
}
const std::string& Value::as_string() const {
  return type_ == Type::String ? s_ : kEmptyString;
}

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& m : obj_) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

void Value::set(std::string key, Value v) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    obj_.clear();
  }
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
}

std::size_t Value::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

std::string escape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char ch : in) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

void Value::dump_into(std::string& out, int indent, int level) const {
  switch (type_) {
    case Type::Null:
      out += "null";
      return;
    case Type::Bool:
      out += b_ ? "true" : "false";
      return;
    case Type::Int: {
      out += std::to_string(i_);
      return;
    }
    case Type::Double: {
      if (!std::isfinite(d_)) {
        // JSON has no NaN/Infinity. Emitting null keeps the document valid and
        // keeps "unknown" unknown instead of inventing a number.
        out += "null";
        return;
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", d_);
      out += buf;
      return;
    }
    case Type::String:
      out.push_back('"');
      out += escape(s_);
      out.push_back('"');
      return;
    case Type::Array: {
      if (arr_.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const auto& v : arr_) {
        if (!first) out.push_back(',');
        first = false;
        write_indent(out, indent, level + 1);
        v.dump_into(out, indent, level + 1);
      }
      write_indent(out, indent, level);
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (obj_.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& m : obj_) {
        if (!first) out.push_back(',');
        first = false;
        write_indent(out, indent, level + 1);
        out.push_back('"');
        out += escape(m.first);
        out += indent < 0 ? "\":" : "\": ";
        m.second.dump_into(out, indent, level + 1);
      }
      write_indent(out, indent, level);
      out.push_back('}');
      return;
    }
  }
}

std::string Value::dump(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  return out;
}

std::optional<Value> parse(std::string_view text, const Limits& limits,
                           ParseError* err) {
  if (text.size() > limits.max_bytes) {
    if (err) {
      err->message = "input exceeds max_bytes";
      err->offset = 0;
      err->line = 0;
    }
    return std::nullopt;
  }
  Parser p{text, 0, limits, err};
  // Tolerate a UTF-8 BOM: some tool exports include one.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    p.pos = 3;
  }
  Value v;
  if (!p.parse_value(v, 0)) return std::nullopt;
  p.skip_ws();
  if (p.pos != text.size()) {
    p.fail("trailing content after top-level value");
    return std::nullopt;
  }
  return v;
}

std::optional<Value> parse_file(const std::string& path, const Limits& limits,
                                ParseError* err) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (err) err->message = "cannot open file: " + path;
    return std::nullopt;
  }
  const std::streamoff size = f.tellg();
  if (size < 0) {
    if (err) err->message = "cannot size file: " + path;
    return std::nullopt;
  }
  if (static_cast<std::size_t>(size) > limits.max_bytes) {
    if (err) err->message = "file exceeds max_bytes: " + path;
    return std::nullopt;
  }
  std::string buf;
  buf.resize(static_cast<std::size_t>(size));
  f.seekg(0);
  if (size > 0 && !f.read(buf.data(), size)) {
    if (err) err->message = "read failed: " + path;
    return std::nullopt;
  }
  return parse(buf, limits, err);
}

// ---------------------------------------------------------------------------
// StreamParser
// ---------------------------------------------------------------------------

struct StreamParser::Impl {
  std::string owned;   // unused for string_view input, kept for lifetime parity
  std::string_view text;
  Limits limits;
  ParseError err;
  Parser parser;
  bool have_error = false;
  // Tracks whether we are positioned before the first element of an array
  // opened with array_begin(), so next_array_element() knows not to expect a
  // leading comma.
  bool array_first_element = false;
  bool array_open = false;
  bool object_open = false;
  bool object_first_member = false;

  Impl(std::string_view t, Limits l)
      : text(t), limits(l), parser{text, 0, limits, &err} {}

  bool fail(const char* msg) {
    if (!have_error) {
      err.message = msg;
      err.offset = parser.pos;
      std::size_t line = 1;
      for (std::size_t i = 0; i < parser.pos && i < text.size(); ++i) {
        if (text[i] == '\n') ++line;
      }
      err.line = line;
    }
    have_error = true;
    return false;
  }
};

StreamParser::StreamParser(std::string_view text, Limits limits)
    : impl_(std::make_unique<Impl>(text, limits)) {
  if (text.size() > limits.max_bytes) {
    impl_->fail("input exceeds max_bytes");
    return;
  }
  // Tolerate a UTF-8 BOM, as parse() does.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    impl_->parser.pos = 3;
  }
}

StreamParser::~StreamParser() = default;

bool StreamParser::failed() const { return impl_->have_error; }
const ParseError& StreamParser::error() const { return impl_->err; }

bool StreamParser::object_begin() {
  if (impl_->have_error) return false;
  auto& p = impl_->parser;
  p.skip_ws();
  if (p.pos >= impl_->text.size() || impl_->text[p.pos] != '{') {
    return impl_->fail("expected '{' at the start of the document");
  }
  ++p.pos;
  impl_->object_open = true;
  impl_->object_first_member = true;
  return true;
}

bool StreamParser::next_member(std::string& key) {
  if (impl_->have_error || !impl_->object_open) return false;
  auto& p = impl_->parser;
  p.skip_ws();
  if (p.pos >= impl_->text.size()) {
    impl_->fail("unterminated object");
    return false;
  }
  if (impl_->text[p.pos] == '}') {
    ++p.pos;
    impl_->object_open = false;
    return false;
  }
  if (!impl_->object_first_member) {
    if (impl_->text[p.pos] != ',') {
      impl_->fail("expected ',' or '}' between members");
      return false;
    }
    ++p.pos;
    p.skip_ws();
  }
  impl_->object_first_member = false;
  key.clear();
  if (!p.parse_string(key)) {
    impl_->have_error = true;
    return false;
  }
  p.skip_ws();
  if (p.pos >= impl_->text.size() || impl_->text[p.pos] != ':') {
    impl_->fail("expected ':' after member name");
    return false;
  }
  ++p.pos;
  return true;
}

bool StreamParser::read_value(Value& out) {
  if (impl_->have_error) return false;
  out = Value::null();
  if (!impl_->parser.parse_value(out, 0)) {
    impl_->have_error = true;
    return false;
  }
  return true;
}

bool StreamParser::skip_value() {
  if (impl_->have_error) return false;
  if (!impl_->parser.skip_value(0)) {
    impl_->have_error = true;
    return false;
  }
  return true;
}

bool StreamParser::array_begin() {
  if (impl_->have_error) return false;
  auto& p = impl_->parser;
  p.skip_ws();
  if (p.pos >= impl_->text.size() || impl_->text[p.pos] != '[') {
    return impl_->fail("expected '[' for an array member");
  }
  ++p.pos;
  impl_->array_open = true;
  impl_->array_first_element = true;
  return true;
}

bool StreamParser::next_array_element(Value& out) {
  if (impl_->have_error || !impl_->array_open) return false;
  auto& p = impl_->parser;
  p.skip_ws();
  if (p.pos >= impl_->text.size()) {
    impl_->fail("unterminated array");
    return false;
  }
  if (impl_->text[p.pos] == ']') {
    ++p.pos;
    impl_->array_open = false;
    return false;
  }
  if (!impl_->array_first_element) {
    if (impl_->text[p.pos] != ',') {
      impl_->fail("expected ',' or ']' between array elements");
      return false;
    }
    ++p.pos;
  }
  impl_->array_first_element = false;
  // Reassigning rather than clearing in place: Value has no reset, and a fresh
  // assignment releases the previous element's storage.
  out = Value::null();
  if (!p.parse_value(out, 0)) {
    impl_->have_error = true;
    return false;
  }
  return true;
}

bool StreamParser::array_skip_rest() {
  if (impl_->have_error || !impl_->array_open) return !impl_->have_error;
  auto& p = impl_->parser;
  for (;;) {
    p.skip_ws();
    if (p.pos >= impl_->text.size()) return impl_->fail("unterminated array");
    if (impl_->text[p.pos] == ']') {
      ++p.pos;
      impl_->array_open = false;
      return true;
    }
    if (!impl_->array_first_element) {
      if (impl_->text[p.pos] != ',') {
        return impl_->fail("expected ',' or ']' between array elements");
      }
      ++p.pos;
    }
    impl_->array_first_element = false;
    if (!p.skip_value(0)) {
      impl_->have_error = true;
      return false;
    }
  }
}

}  // namespace mpi::json
