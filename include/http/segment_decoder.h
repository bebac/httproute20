#ifndef HTTPROUTE20_SEGMENT_DECODER_H
#define HTTPROUTE20_SEGMENT_DECODER_H

#include <concepts>
#include <cstddef>
#include <optional>
#include <string_view>

namespace http
{
  // A segment decoder rewrites one path segment before the router
  // compares it against a pattern literal or hands it to a handler as
  // a capture — percent-decoding in the typical case. The router calls
  // decode only when a segment contains '%', so escape-free requests
  // never pay for it. decode writes into scratch (capacity bytes) and
  // returns a view into that storage, or nullopt when the segment
  // cannot be decoded: malformed input, or scratch too small.
  //
  // Contract: pure — the same input always yields the same result, no
  // side effects — and the returned view is valid only while scratch
  // lives.
  template<typename D>
  concept segment_decoder = requires(const D& d, std::string_view segment, char* scratch, size_t capacity) {
    { d.decode(segment, scratch, capacity) } -> std::convertible_to<std::optional<std::string_view>>;
  };

  // The default decoder: no decoding at all. The router matches and
  // captures the raw path exactly as given — the library's
  // zero-dependency behavior, and the behavior every router had
  // before decoding was a parameter.
  struct identity_segment_decoder
  {
    std::optional<std::string_view> decode(std::string_view segment, char*, size_t) const
    {
      return segment;
    }
  };

  // Scratch bound for one decoded segment during literal matching, and
  // the arena bound for all decoded captures of one request during
  // dispatch. Fixed so matching and dispatch stay allocation-free. A
  // segment that does not fit simply does not match its literal; a
  // capture set that does not fit reports bad_path through the
  // param-error policy.
  inline constexpr size_t decode_scratch_size = 512;
  inline constexpr size_t capture_arena_size = 1024;
} // namespace http

#endif // HTTPROUTE20_SEGMENT_DECODER_H
