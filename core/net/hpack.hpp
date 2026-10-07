// HPACK (RFC 7541): the header compression HTTP/2 requires.
//
// The decoder is complete -- static and dynamic tables, every representation,
// table size updates, and Huffman-coded strings -- because the peer chooses how
// to encode and a gRPC server does use Huffman and the dynamic table. The
// encoder is deliberately minimal: every header goes out as a literal without
// indexing, which every decoder must accept and which keeps the encoder free of
// state that could drift from the peer's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpi::net {

using Header = std::pair<std::string, std::string>;
using Headers = std::vector<Header>;

class HpackDecoder {
 public:
  /// `max_table_size` is what this side advertised in SETTINGS_HEADER_TABLE_SIZE;
  /// the peer may shrink the table below it but never grow it past.
  explicit HpackDecoder(std::size_t max_table_size = 4096) : limit_(max_table_size) {}
  /// Decodes one complete header block. False on malformed input, with `error()`.
  bool decode(std::string_view block, Headers& out);
  const std::string& error() const { return error_; }

 private:
  bool lookup(std::uint64_t index, Header& out);
  void insert(Header h);
  void evict_to(std::size_t bytes);
  std::deque<Header> dynamic_;
  std::size_t size_ = 0;
  std::size_t max_ = 4096;
  std::size_t limit_;
  std::string error_;
};

/// One header block, every header a literal without indexing.
std::string hpack_encode(const Headers& headers);

/// Huffman coding of a string (RFC 7541 Appendix B), exposed for the tests.
std::string huffman_encode(std::string_view in);
bool huffman_decode(std::string_view in, std::string& out);

}  // namespace mpi::net
