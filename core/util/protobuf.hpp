// The protobuf wire format, written and read by hand.
//
// Enough of it for gRPC calls to the Android emulator: varints, 32- and 64-bit
// fixed fields, and length-delimited fields (strings, bytes, nested messages).
// There is no schema compiler here, by the same rule that keeps every other
// library out of the tree (ADR-0002): a caller names its field numbers, and
// the proto file they come from is cited beside the call.
//
// Reading is bounded: a length that runs past the buffer, a varint longer than
// ten bytes, or a wire type this reader does not know stops the read and
// reports it, rather than reading past the end of a message.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mpi::pb {

enum class WireType : std::uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kFixed32 = 5,
};

class Writer {
 public:
  void varint(std::uint32_t field, std::uint64_t value);
  /// Zigzag is not used by the emulator's protos; `int32` fields are plain
  /// varints, with a negative value sign-extended to ten bytes as protobuf
  /// specifies.
  void int32(std::uint32_t field, std::int32_t value);
  void boolean(std::uint32_t field, bool value) { varint(field, value ? 1 : 0); }
  void fixed64(std::uint32_t field, std::uint64_t value);
  void fixed32(std::uint32_t field, std::uint32_t value);
  void dbl(std::uint32_t field, double value);
  void flt(std::uint32_t field, float value);
  void bytes(std::uint32_t field, std::string_view value);
  void string(std::uint32_t field, std::string_view value) { bytes(field, value); }
  void message(std::uint32_t field, const Writer& nested) { bytes(field, nested.data()); }

  const std::string& data() const { return out_; }

 private:
  void tag(std::uint32_t field, WireType type);
  void raw_varint(std::uint64_t v);
  std::string out_;
};

struct Field {
  std::uint32_t number = 0;
  WireType type = WireType::kVarint;
  std::uint64_t value = 0;     // varint and fixed fields
  std::string_view bytes;      // length-delimited fields; points into the buffer

  std::int32_t as_int32() const { return static_cast<std::int32_t>(value); }
  double as_double() const;
  float as_float() const;
};

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}
  /// The next field, or false at the end or on malformed input (`error()`).
  bool next(Field& out);
  const std::string& error() const { return error_; }

 private:
  bool read_varint(std::uint64_t& out);
  std::string_view data_;
  std::size_t pos_ = 0;
  std::string error_;
};

}  // namespace mpi::pb
