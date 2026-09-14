#include "core/util/xml.hpp"

#include <cstring>

namespace mpi::xml {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// XML name characters, narrowed to what the export format actually uses.
// Being strict here is the point: a name character set that accepts anything
// would let a malformed document look like a valid one.
bool is_name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c == ':' || static_cast<unsigned char>(c) >= 0x80;
}

bool is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

// A local substring search rather than `memmem`, which is a BSD extension and
// not in the standard library.
const char* find_bytes(const char* haystack, std::size_t haystack_len,
                       const char* needle, std::size_t needle_len) {
  if (needle_len == 0 || haystack_len < needle_len) return nullptr;
  const std::size_t last = haystack_len - needle_len;
  for (std::size_t i = 0; i <= last; ++i) {
    if (std::memcmp(haystack + i, needle, needle_len) == 0) {
      return haystack + i;
    }
  }
  return nullptr;
}

}  // namespace

Parser::Parser(const char* data, std::size_t size, Limits limits)
    : data_(data), size_(size), limits_(limits) {
  if (size_ > limits_.max_bytes) {
    error_ = "input is " + std::to_string(size_) +
             " bytes, over the configured limit of " +
             std::to_string(limits_.max_bytes);
  }
}

bool Parser::fail(const std::string& message) {
  if (error_.empty()) {
    error_ = message + " at byte " + std::to_string(pos_);
  }
  kind_ = NodeKind::kNone;
  return false;
}

void Parser::skip_space() {
  while (pos_ < size_ && is_space(data_[pos_])) ++pos_;
}

bool Parser::read_name(std::string& out) {
  out.clear();
  if (pos_ >= size_ || !is_name_start(data_[pos_])) {
    return fail("expected an element or attribute name");
  }
  while (pos_ < size_ && is_name_char(data_[pos_])) {
    if (out.size() >= limits_.max_name_length) {
      return fail("name exceeds the configured length limit");
    }
    out.push_back(data_[pos_++]);
  }
  return true;
}

bool Parser::decode_entities(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out.push_back(in[i]);
      continue;
    }
    const auto semi = in.find(';', i + 1);
    // A bounded search: an unterminated '&' is a malformed document, not an
    // invitation to scan to the end of a 400 MB file.
    if (semi == std::string::npos || semi - i > 16) {
      return fail("unterminated entity reference");
    }
    const std::string name = in.substr(i + 1, semi - i - 1);
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (name.size() > 1 && name[0] == '#') {
      // Numeric character references, which xctrace emits for characters that
      // appear in C++ symbol names.
      std::uint32_t code = 0;
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const std::string digits = hex ? name.substr(2) : name.substr(1);
      if (digits.empty()) return fail("empty numeric character reference");
      for (const char c : digits) {
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9') {
          d = static_cast<std::uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
          d = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
          d = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
          return fail("malformed numeric character reference");
        }
        code = code * (hex ? 16u : 10u) + d;
        if (code > 0x10FFFF) return fail("character reference out of range");
      }
      // Encoded as UTF-8 so downstream string handling stays byte-oriented.
      if (code < 0x80) {
        out.push_back(static_cast<char>(code));
      } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      }
    } else {
      // An unknown entity is refused rather than passed through: a document
      // that declares its own entities is exactly what this parser will not
      // process.
      return fail("unknown entity reference '&" + name + ";'");
    }
    i = semi;
  }
  return true;
}

bool Parser::read_attribute_value(std::string& out) {
  if (pos_ >= size_ || (data_[pos_] != '"' && data_[pos_] != '\'')) {
    return fail("attribute value must be quoted");
  }
  const char quote = data_[pos_++];
  std::string raw;
  while (pos_ < size_ && data_[pos_] != quote) {
    if (raw.size() >= limits_.max_text_length) {
      return fail("attribute value exceeds the configured length limit");
    }
    raw.push_back(data_[pos_++]);
  }
  if (pos_ >= size_) return fail("unterminated attribute value");
  ++pos_;  // closing quote
  return decode_entities(raw, out);
}

bool Parser::handle_bang_or_question() {
  // `<?xml ...?>`, `<!-- ... -->`, `<![CDATA[...]]>`; `<!DOCTYPE` is refused.
  if (pos_ + 1 >= size_) return fail("truncated markup declaration");
  if (data_[pos_ + 1] == '?') {
    const char* end = find_bytes(data_ + pos_, size_ - pos_, "?>", 2);
    if (end == nullptr) return fail("unterminated processing instruction");
    pos_ = static_cast<std::size_t>(end - data_) + 2;
    return true;
  }
  if (size_ - pos_ >= 4 && std::memcmp(data_ + pos_, "<!--", 4) == 0) {
    const char* end = find_bytes(data_ + pos_ + 4, size_ - pos_ - 4, "-->", 3);
    if (end == nullptr) return fail("unterminated comment");
    pos_ = static_cast<std::size_t>(end - data_) + 3;
    return true;
  }
  if (size_ - pos_ >= 9 && std::memcmp(data_ + pos_, "<![CDATA[", 9) == 0) {
    const char* end = find_bytes(data_ + pos_ + 9, size_ - pos_ - 9, "]]>", 3);
    if (end == nullptr) return fail("unterminated CDATA section");
    const auto start = pos_ + 9;
    const auto len = static_cast<std::size_t>(end - data_) - start;
    if (len > limits_.max_text_length) {
      return fail("CDATA section exceeds the configured length limit");
    }
    text_.assign(data_ + start, len);
    name_.clear();
    attributes_.clear();
    self_closing_ = false;
    kind_ = NodeKind::kText;
    pos_ = static_cast<std::size_t>(end - data_) + 3;
    return true;
  }
  // Any other markup declaration, DOCTYPE included.
  return fail(
      "markup declarations are not processed; a document type declaration is "
      "refused rather than partially honoured");
}

bool Parser::next() {
  if (failed()) return false;
  attributes_.clear();
  name_.clear();
  text_.clear();
  self_closing_ = false;

  for (;;) {
    if (pos_ >= size_) {
      kind_ = NodeKind::kEndOfInput;
      return false;
    }

    if (data_[pos_] != '<') {
      // Text run up to the next '<'.
      const char* found = static_cast<const char*>(
          std::memchr(data_ + pos_, '<', size_ - pos_));
      const std::size_t end =
          found == nullptr ? size_ : static_cast<std::size_t>(found - data_);
      const std::size_t len = end - pos_;
      if (len > limits_.max_text_length) {
        return fail("text node exceeds the configured length limit");
      }
      std::string raw(data_ + pos_, len);
      pos_ = end;
      // Whitespace-only runs are structural, not content: reporting them
      // would make every caller filter them out.
      bool only_space = true;
      for (const char c : raw) {
        if (!is_space(c)) {
          only_space = false;
          break;
        }
      }
      if (only_space) continue;
      if (!decode_entities(raw, text_)) return false;
      kind_ = NodeKind::kText;
      return true;
    }

    if (pos_ + 1 < size_ &&
        (data_[pos_ + 1] == '?' || data_[pos_ + 1] == '!')) {
      const auto before = kind_;
      if (!handle_bang_or_question()) return false;
      if (kind_ == NodeKind::kText && before != NodeKind::kText) return true;
      if (kind_ == NodeKind::kText) return true;
      continue;
    }

    ++pos_;  // '<'
    if (pos_ < size_ && data_[pos_] == '/') {
      ++pos_;
      if (!read_name(name_)) return false;
      skip_space();
      if (pos_ >= size_ || data_[pos_] != '>') {
        return fail("expected '>' to close an end tag");
      }
      ++pos_;
      if (depth_ == 0) return fail("end tag with no open element");
      --depth_;
      kind_ = NodeKind::kEndElement;
      return true;
    }

    if (!read_name(name_)) return false;
    for (;;) {
      skip_space();
      if (pos_ >= size_) return fail("unterminated start tag");
      if (data_[pos_] == '>') {
        ++pos_;
        if (depth_ >= limits_.max_depth) {
          return fail("element nesting exceeds the configured depth limit");
        }
        ++depth_;
        kind_ = NodeKind::kStartElement;
        return true;
      }
      if (data_[pos_] == '/') {
        ++pos_;
        if (pos_ >= size_ || data_[pos_] != '>') {
          return fail("expected '>' after '/' in a self-closing tag");
        }
        ++pos_;
        // Checked here as well as on the open-tag path: a self-closing
        // element still sits at a nesting level, and a limit that means one
        // thing for `<a></a>` and another for `<a/>` is not a limit.
        if (depth_ >= limits_.max_depth) {
          return fail("element nesting exceeds the configured depth limit");
        }
        self_closing_ = true;
        kind_ = NodeKind::kStartElement;
        return true;
      }
      if (attributes_.size() >= limits_.max_attributes) {
        return fail("element has more attributes than the configured limit");
      }
      Attribute attr;
      if (!read_name(attr.name)) return false;
      skip_space();
      if (pos_ >= size_ || data_[pos_] != '=') {
        return fail("expected '=' after an attribute name");
      }
      ++pos_;
      skip_space();
      if (!read_attribute_value(attr.value)) return false;
      attributes_.push_back(std::move(attr));
    }
  }
}

std::optional<std::string> Parser::attribute(const std::string& key) const {
  for (const auto& a : attributes_) {
    if (a.name == key) return a.value;
  }
  return std::nullopt;
}

bool Parser::skip_element() {
  if (kind_ != NodeKind::kStartElement) return true;
  if (self_closing_) return true;
  const std::size_t target = depth_ - 1;
  while (next()) {
    if (kind_ == NodeKind::kEndElement && depth_ == target) return true;
  }
  return !failed();
}

}  // namespace mpi::xml
