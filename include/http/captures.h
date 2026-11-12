#ifndef HTTPPROUTE20_CAPTURES_H
#define HTTPPROUTE20_CAPTURES_H

#include <array>
#include <cstddef>
#include <string_view>

namespace http
{
  // Upper bound on the number of path segments in a pattern, and on how
  // many request-path segments matching looks at. Fixed so that matching
  // and capture collection need no heap. A request path may be longer:
  // the segments past this bound are reachable only through a catch-all,
  // which captures the raw remainder rather than single segments.
  inline constexpr size_t max_segments = 16;

  // One ":name" segment captured while walking the route trie. `name`
  // points into the pattern literal and `value` into the request path, so
  // both are only valid for the duration of the dispatch.
  struct capture
  {
    std::string_view name;
    std::string_view value;
  };

  // Captures in pattern order — the order the handler's trailing
  // parameters expect.
  struct capture_list
  {
    std::array<capture, max_segments> items{};
    size_t count = 0;

    void push(capture value)
    {
      if (count < max_segments)
        items[count++] = value;
    }

    void pop()
    {
      if (count > 0)
        --count;
    }

    const capture& operator[](size_t index) const
    {
      return items[index];
    }

    size_t size() const
    {
      return count;
    }

    // Iteration covers only the filled prefix, so capture_list is a
    // range over the captures actually collected — not the whole
    // fixed-size array behind them.
    const capture* begin() const
    {
      return items.data();
    }

    const capture* end() const
    {
      return items.data() + count;
    }
  };
} // namespace http

#endif // HTTPPROUTE20_CAPTURES_H
