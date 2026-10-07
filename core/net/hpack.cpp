#include "core/net/hpack.hpp"

#include <array>

#include "core/net/hpack_tables.hpp"

namespace mpi::net {
namespace {

using hpack_tables::kHuffmanCode;
using hpack_tables::kHuffmanLength;

// Canonical decoding: for each code length, the first code of that length and
// where its symbols start in the length-then-symbol ordering.
struct Canonical {
  std::array<std::uint32_t, 31> first{};
  std::array<std::uint32_t, 31> count{};
  std::array<std::uint32_t, 31> offset{};
  std::array<std::uint16_t, 257> symbols{};

  Canonical() {
    for (std::size_t s = 0; s < 257; s++) count[kHuffmanLength[s]]++;
    std::uint32_t code = 0, pos = 0;
    for (std::size_t len = 1; len <= 30; len++) {
      code <<= 1;
      first[len] = code;
      offset[len] = pos;
      code += count[len];
      pos += count[len];
    }
    std::array<std::uint32_t, 31> fill{};
    for (std::size_t len = 1; len <= 30; len++) {
      for (std::size_t s = 0; s < 257; s++) {
        if (kHuffmanLength[s] == len) {
          symbols[offset[len] + fill[len]++] = static_cast<std::uint16_t>(s);
        }
      }
    }
  }
};

const Canonical& canonical() {
  static const Canonical c;
  return c;
}

bool read_int(std::string_view in, std::size_t& pos, int prefix_bits, std::uint64_t& out) {
  if (pos >= in.size()) return false;
  const std::uint8_t mask = static_cast<std::uint8_t>((1u << prefix_bits) - 1);
  out = static_cast<std::uint8_t>(in[pos++]) & mask;
  if (out < mask) return true;
  for (int shift = 0; shift < 56; shift += 7) {
    if (pos >= in.size()) return false;
    const auto b = static_cast<std::uint8_t>(in[pos++]);
    out += static_cast<std::uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return true;
  }
  return false;
}

bool read_string(std::string_view in, std::size_t& pos, std::string& out) {
  if (pos >= in.size()) return false;
  const bool huffman = (static_cast<std::uint8_t>(in[pos]) & 0x80) != 0;
  std::uint64_t len = 0;
  if (!read_int(in, pos, 7, len) || len > in.size() - pos) return false;
  const auto raw = in.substr(pos, static_cast<std::size_t>(len));
  pos += static_cast<std::size_t>(len);
  if (!huffman) {
    out.assign(raw.data(), raw.size());
    return true;
  }
  return huffman_decode(raw, out);
}

void write_int(std::string& out, std::uint8_t first_bits, int prefix_bits, std::uint64_t v) {
  const std::uint64_t mask = (1u << prefix_bits) - 1;
  if (v < mask) {
    out.push_back(static_cast<char>(first_bits | v));
    return;
  }
  out.push_back(static_cast<char>(first_bits | mask));
  v -= mask;
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

void write_string(std::string& out, std::string_view s) {
  write_int(out, 0x00, 7, s.size());
  out.append(s.data(), s.size());
}

}  // namespace

std::string huffman_encode(std::string_view in) {
  std::string out;
  std::uint64_t acc = 0;
  int bits = 0;
  for (char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    acc = (acc << kHuffmanLength[c]) | kHuffmanCode[c];
    bits += kHuffmanLength[c];
    while (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  if (bits > 0) {
    // Padded with the most significant bits of EOS, which are all ones.
    out.push_back(static_cast<char>(((acc << (8 - bits)) | (0xffu >> bits)) & 0xff));
  }
  return out;
}

bool huffman_decode(std::string_view in, std::string& out) {
  const auto& c = canonical();
  out.clear();
  std::uint32_t code = 0;
  int len = 0;
  for (std::size_t i = 0; i < in.size(); i++) {
    const auto byte = static_cast<std::uint8_t>(in[i]);
    for (int b = 7; b >= 0; b--) {
      code = (code << 1) | ((byte >> b) & 1u);
      len++;
      if (len > 30) return false;
      if (c.count[static_cast<std::size_t>(len)] != 0 &&
          code - c.first[static_cast<std::size_t>(len)] < c.count[static_cast<std::size_t>(len)]) {
        const auto sym = c.symbols[c.offset[static_cast<std::size_t>(len)] +
                                   (code - c.first[static_cast<std::size_t>(len)])];
        if (sym == 256) return false;  // EOS inside a string is an error
        out.push_back(static_cast<char>(sym));
        code = 0;
        len = 0;
      }
    }
  }
  // Padding: fewer than eight bits, all ones (a prefix of EOS).
  return len < 8 && code == ((1u << len) - 1);
}

bool HpackDecoder::lookup(std::uint64_t index, Header& out) {
  if (index == 0) return false;
  if (index <= 61) {
    out = {hpack_tables::kStatic[index - 1].name, hpack_tables::kStatic[index - 1].value};
    return true;
  }
  const std::uint64_t d = index - 62;
  if (d >= dynamic_.size()) return false;
  out = dynamic_[static_cast<std::size_t>(d)];
  return true;
}

void HpackDecoder::evict_to(std::size_t bytes) {
  while (!dynamic_.empty() && size_ > bytes) {
    size_ -= dynamic_.back().first.size() + dynamic_.back().second.size() + 32;
    dynamic_.pop_back();
  }
}

void HpackDecoder::insert(Header h) {
  const std::size_t entry = h.first.size() + h.second.size() + 32;
  if (entry > max_) {  // larger than the table: it empties it and is not kept
    evict_to(0);
    return;
  }
  evict_to(max_ - entry);
  size_ += entry;
  dynamic_.push_front(std::move(h));
}

bool HpackDecoder::decode(std::string_view in, Headers& out) {
  std::size_t pos = 0;
  while (pos < in.size()) {
    const auto b = static_cast<std::uint8_t>(in[pos]);
    std::uint64_t index = 0;
    if (b & 0x80) {  // indexed
      Header h;
      if (!read_int(in, pos, 7, index) || !lookup(index, h)) {
        error_ = "bad indexed header field";
        return false;
      }
      out.push_back(std::move(h));
      continue;
    }
    if ((b & 0xe0) == 0x20) {  // dynamic table size update
      if (!read_int(in, pos, 5, index) || index > limit_) {
        error_ = "bad table size update";
        return false;
      }
      max_ = static_cast<std::size_t>(index);
      evict_to(max_);
      continue;
    }
    const bool incremental = (b & 0xc0) == 0x40;
    const int prefix = incremental ? 6 : 4;
    if (!read_int(in, pos, prefix, index)) {
      error_ = "bad literal header field";
      return false;
    }
    Header h;
    if (index == 0) {
      if (!read_string(in, pos, h.first)) {
        error_ = "bad header name";
        return false;
      }
    } else {
      Header named;
      if (!lookup(index, named)) {
        error_ = "bad header name index";
        return false;
      }
      h.first = named.first;
    }
    if (!read_string(in, pos, h.second)) {
      error_ = "bad header value";
      return false;
    }
    if (incremental) insert(h);
    out.push_back(std::move(h));
  }
  return true;
}

std::string hpack_encode(const Headers& headers) {
  std::string out;
  for (const auto& [name, value] : headers) {
    out.push_back(0x00);  // literal without indexing, new name
    write_string(out, name);
    write_string(out, value);
  }
  return out;
}

}  // namespace mpi::net
