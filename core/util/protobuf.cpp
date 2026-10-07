#include "core/util/protobuf.hpp"

#include <cstring>

namespace mpi::pb {

void Writer::raw_varint(std::uint64_t v) {
  while (v >= 0x80) {
    out_.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out_.push_back(static_cast<char>(v));
}

void Writer::tag(std::uint32_t field, WireType type) {
  raw_varint((static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint8_t>(type));
}

void Writer::varint(std::uint32_t field, std::uint64_t value) {
  tag(field, WireType::kVarint);
  raw_varint(value);
}

void Writer::int32(std::uint32_t field, std::int32_t value) {
  // Sign-extended: a negative int32 is encoded as the 64-bit two's complement.
  varint(field, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void Writer::fixed64(std::uint32_t field, std::uint64_t value) {
  tag(field, WireType::kFixed64);
  for (int i = 0; i < 8; i++) out_.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void Writer::fixed32(std::uint32_t field, std::uint32_t value) {
  tag(field, WireType::kFixed32);
  for (int i = 0; i < 4; i++) out_.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void Writer::dbl(std::uint32_t field, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  fixed64(field, bits);
}

void Writer::flt(std::uint32_t field, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  fixed32(field, bits);
}

void Writer::bytes(std::uint32_t field, std::string_view value) {
  tag(field, WireType::kLengthDelimited);
  raw_varint(value.size());
  out_.append(value.data(), value.size());
}

double Field::as_double() const {
  double d = 0;
  std::memcpy(&d, &value, sizeof d);
  return d;
}

float Field::as_float() const {
  const auto bits = static_cast<std::uint32_t>(value);
  float f = 0;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

bool Reader::read_varint(std::uint64_t& out) {
  out = 0;
  for (int shift = 0; shift < 70; shift += 7) {
    if (pos_ >= data_.size()) {
      error_ = "a varint runs past the end of the message";
      return false;
    }
    const auto b = static_cast<std::uint8_t>(data_[pos_++]);
    out |= static_cast<std::uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return true;
  }
  error_ = "a varint is longer than ten bytes";
  return false;
}

bool Reader::next(Field& out) {
  if (pos_ >= data_.size()) return false;
  std::uint64_t key = 0;
  if (!read_varint(key)) return false;
  out = Field{};
  out.number = static_cast<std::uint32_t>(key >> 3);
  const auto wt = static_cast<std::uint8_t>(key & 7);
  switch (wt) {
    case 0:
      out.type = WireType::kVarint;
      return read_varint(out.value);
    case 1:
    case 5: {
      const std::size_t n = wt == 1 ? 8 : 4;
      if (data_.size() - pos_ < n) {
        error_ = "a fixed field runs past the end of the message";
        return false;
      }
      out.type = wt == 1 ? WireType::kFixed64 : WireType::kFixed32;
      for (std::size_t i = 0; i < n; i++) {
        out.value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[pos_ + i]))
                     << (8 * i);
      }
      pos_ += n;
      return true;
    }
    case 2: {
      std::uint64_t len = 0;
      if (!read_varint(len)) return false;
      if (len > data_.size() - pos_) {
        error_ = "a length-delimited field runs past the end of the message";
        return false;
      }
      out.type = WireType::kLengthDelimited;
      out.bytes = data_.substr(pos_, static_cast<std::size_t>(len));
      pos_ += static_cast<std::size_t>(len);
      return true;
    }
    default:
      error_ = "wire type " + std::to_string(wt) + " is not one this reader knows";
      return false;
  }
}

}  // namespace mpi::pb
