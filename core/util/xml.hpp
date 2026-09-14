// A bounded XML pull parser.
//
// It exists for one input: `xcrun xctrace export`, whose XML is the only
// supported machine interface to an Instruments trace. Writing one is
// cheaper than taking on a dependency (ADR-0002) and, more importantly, the
// parser has to treat its input as hostile in the same way the JSON one does:
// a trace file arrives from a device and a build machine, not from us.
//
// It is deliberately a *pull* parser. An xctrace time-profile export is
// hundreds of megabytes for a real capture, and a DOM of it would repeat the
// 10x memory blow-up the JSON path already had to fix.
//
// What it does not do, on purpose: no DTDs, no entity declarations, no
// namespace resolution, no XPath. A DTD is an attack surface (billion laughs)
// with no use here, so an input that declares one is refused rather than
// partially honoured.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpi::xml {

struct Limits {
  // 2 GiB, matching the JSON reader: the spec's own stress fixture is 1 GiB
  // and a real Instruments export is larger still.
  std::size_t max_bytes = 2ull * 1024 * 1024 * 1024;
  std::size_t max_depth = 256;
  std::size_t max_attributes = 64;
  std::size_t max_name_length = 1024;
  // Per text node and per attribute value.
  std::size_t max_text_length = 16ull * 1024 * 1024;
};

struct Attribute {
  std::string name;
  std::string value;
};

// What the cursor is sitting on.
enum class NodeKind {
  kNone,
  kStartElement,
  kEndElement,
  // An element written as `<x/>`: reported once as a start element with
  // `self_closing` set, and never followed by an end element.
  kText,
  kEndOfInput,
};

class Parser {
 public:
  Parser(const char* data, std::size_t size, Limits limits = Limits());

  // Advances to the next node. Returns false at end of input or on the first
  // error; `error()` says which.
  bool next();

  NodeKind kind() const { return kind_; }
  const std::string& name() const { return name_; }
  const std::string& text() const { return text_; }
  bool self_closing() const { return self_closing_; }
  const std::vector<Attribute>& attributes() const { return attributes_; }
  // Attribute lookup by name; absent rather than empty when missing, because
  // `addr=""` and no `addr` at all are different facts.
  std::optional<std::string> attribute(const std::string& key) const;

  std::size_t depth() const { return depth_; }
  const std::string& error() const { return error_; }
  bool failed() const { return !error_.empty(); }
  std::size_t offset() const { return pos_; }

  // Skips everything up to and including the end of the element the cursor is
  // currently on. Used to walk past a subtree cheaply.
  bool skip_element();

 private:
  bool fail(const std::string& message);
  void skip_space();
  bool read_name(std::string& out);
  bool read_attribute_value(std::string& out);
  bool decode_entities(const std::string& in, std::string& out);
  bool handle_bang_or_question();

  const char* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  Limits limits_;

  NodeKind kind_ = NodeKind::kNone;
  std::string name_;
  std::string text_;
  bool self_closing_ = false;
  std::vector<Attribute> attributes_;
  std::size_t depth_ = 0;
  std::string error_;
};

}  // namespace mpi::xml
