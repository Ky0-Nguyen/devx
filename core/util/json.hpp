// Minimal, bounded JSON value / parser / serializer.
//
// Deliberately dependency-free: the third-party license inventory required by
// the specification stays empty for the core, and every limit demanded by
// spec section 15 (bounded parser memory, depth, container size) is enforced
// here rather than hoped for.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpi::json {

class Value;

using Member = std::pair<std::string, Value>;

// Hard limits applied while parsing untrusted input (traces, symbol maps,
// session packages). Exceeding any limit is a parse error, never a truncation.
struct Limits {
  // 2 GiB. Spec section 15 requires a 1 GiB trace to be processable (and spec
  // I21 requires the UI to stay responsive under it), so the default ceiling
  // has to sit comfortably above that rather than at it. It is still a hard
  // bound: an input past this is refused, never partially read.
  std::size_t max_bytes = 2ull * 1024ull * 1024ull * 1024ull;
  std::size_t max_depth = 128;
  std::size_t max_container_elements = 8ull * 1024ull * 1024ull;
  std::size_t max_string_bytes = 64ull * 1024ull * 1024ull;
};

struct ParseError {
  std::string message;
  std::size_t offset = 0;
  std::size_t line = 0;
};

class Value {
 public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };

  Value() = default;
  static Value null() { return Value(); }
  static Value boolean(bool v);
  static Value integer(std::int64_t v);
  static Value number(double v);
  static Value string(std::string v);
  static Value array(std::vector<Value> v = {});
  static Value object(std::vector<Member> v = {});

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_int() const { return type_ == Type::Int; }
  bool is_double() const { return type_ == Type::Double; }
  bool is_number() const { return is_int() || is_double(); }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  // Accessors return a default rather than throwing: no C++ exception may
  // cross the C ABI / JNI boundaries described in spec section 5.
  bool as_bool(bool fallback = false) const;
  std::int64_t as_int(std::int64_t fallback = 0) const;
  double as_double(double fallback = 0.0) const;
  const std::string& as_string() const;
  const std::vector<Value>& items() const { return arr_; }
  const std::vector<Member>& members() const { return obj_; }

  std::vector<Value>& items() { return arr_; }
  std::vector<Member>& members() { return obj_; }

  // Object field lookup. Returns nullptr when absent -- absent is distinct
  // from present-and-null, which the metric/eligibility model relies on.
  const Value* find(std::string_view key) const;
  bool has(std::string_view key) const { return find(key) != nullptr; }
  void set(std::string key, Value v);
  void push_back(Value v) { arr_.push_back(std::move(v)); }
  std::size_t size() const;

  std::string dump(int indent = -1) const;

 private:
  void dump_into(std::string& out, int indent, int level) const;

  Type type_ = Type::Null;
  bool b_ = false;
  std::int64_t i_ = 0;
  double d_ = 0.0;
  std::string s_;
  std::vector<Value> arr_;
  std::vector<Member> obj_;
};

// Parses `text`. On failure returns nullopt and fills `err`.
std::optional<Value> parse(std::string_view text, const Limits& limits,
                           ParseError* err);
inline std::optional<Value> parse(std::string_view text, ParseError* err) {
  return parse(text, Limits{}, err);
}

// Reads and parses a file, enforcing `limits.max_bytes` before allocating.
std::optional<Value> parse_file(const std::string& path, const Limits& limits,
                                ParseError* err);

// Escapes `in` for embedding in JSON string context.
std::string escape(std::string_view in);

}  // namespace mpi::json
