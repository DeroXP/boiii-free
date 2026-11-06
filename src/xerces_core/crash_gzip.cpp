// gzip for crash report bodies (crash_outbox.hpp): zlib, already in deps for
// the client. Content-Encoding is optional in the contract, so a report whose
// compression fails goes uncompressed.

#include "crash_outbox.hpp"

#ifndef ZLIB_CONST
#define ZLIB_CONST
#endif
#include <zlib.h>

namespace xerces_core {
std::string gzip_bytes(std::string_view bytes) {
  z_stream stream{};
  // 15 window bits plus 16: a gzip header and trailer instead of zlib's.
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 9,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  std::string out(deflateBound(&stream, static_cast<uLong>(bytes.size())) + 32,
                  '\0');
  stream.next_in = reinterpret_cast<const Bytef *>(bytes.data());
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = reinterpret_cast<Bytef *>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  const std::size_t written = stream.total_out;
  deflateEnd(&stream);
  if (result != Z_STREAM_END) {
    return {};
  }
  out.resize(written);
  return out;
}
} // namespace xerces_core
