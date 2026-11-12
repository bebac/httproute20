#ifndef HTTPROUTE20_PATTERN_H
#define HTTPROUTE20_PATTERN_H

#include "http/captures.h"
#include "http/traits.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace http
{
  // Pattern validation: pattern_text (the template-argument syntax) and
  // typed_pattern's consteval constructor split the literal, reject
  // malformed ":name" segments, accept a trailing "*name" catch-all, and
  // check the ":name"/"*name" count against the handler's path parameters.
  // A route pattern as a template argument — a C++20 structural type with
  // a consteval constructor from the literal — for the call syntax
  //
  //   router.get<"/entity/:id">(&get_entity);
  //
  // The literal syntax (router.get("/entity/:id", &get_entity)) keeps the
  // pattern as runtime data: a function parameter can never be a constant
  // expression, so its arity check degrades to a consteval throw. Here the
  // pattern is a template parameter object, so path_param_count() is a
  // constant expression and the arity check is a static_assert that names
  // both counts.
  template<size_t N>
  struct pattern_text
  {
    char value[N]{};

    consteval pattern_text(const char (&text)[N])
    {
      for (size_t i = 0; i < N; ++i)
        value[i] = text[i];
    }

    // The ":name"/"*name" segment count, mirroring typed_pattern's split.
    consteval size_t path_param_count() const
    {
      size_t count = 0;
      size_t start = 0;
      while (start + 1 < N)
      {
        if (value[start] == '/')
        {
          ++start;
          continue;
        }
        size_t end = start;
        while (end + 1 < N && value[end] != '/')
          ++end;
        if (value[start] == ':' || value[start] == '*')
          ++count;
        start = end;
      }
      return count;
    }
  };

  // A route pattern validated at compile time.
  //
  // The consteval constructor splits the pattern, rejects malformed
  // ":name" segments and empty segments (which the splitter would
  // otherwise silently skip, making "/a//b" the same route as "/a/b"),
  // accepts a single trailing "*name" catch-all segment (which must be
  // last and counts as one path parameter), and checks that the
  // ":name"/"*name" count matches the handler's path parameter count
  // (injected parameters are not counted).
  // The results are stored in the object: computed at compile time, but
  // runtime *data* — a function parameter can never be a constant expression,
  // so the pattern cannot become a template argument. That is the price of writing
  //
  //   router.get("/entity/:id", &get_entity);
  //
  // instead of router.get<"/entity/:id">(&get_entity) — the
  // template-argument syntax, where the pattern is a constant expression
  // and the arity check is a static_assert naming both counts (see
  // pattern_text above). With the literal syntax a violating registration
  // is reported as a non-constant consteval call, which names the pattern
  // but not the mismatching counts.
  // Tag selecting typed_pattern's constructor that skips the arity throw:
  // the template-argument syntax (pattern_text) has already checked the
  // counts with a static_assert, so the consteval throw would only double
  // the diagnostic.
  struct unchecked_pattern_t
  {
    explicit unchecked_pattern_t() = default;
  };

  template<typename Handler>
  struct typed_pattern
  {
    template<size_t N>
    consteval typed_pattern(const char (&text)[N]) : text_{text, N - 1}
    {
      split(text);
      if (param_count_ != detail::path_param_count_v<Handler>)
        throw "route pattern ':name'/'*name' count does not match the handler's path parameters";
    }

    template<size_t N>
    consteval typed_pattern(const char (&text)[N], unchecked_pattern_t) : text_{text, N - 1}
    {
      split(text);
    }

    template<size_t N>
    consteval void split(const char (&text)[N])
    {
      // The splitter below silently skips empty segments; reject them so
      // a typo like "/entity//latest" or "/entity/" cannot quietly
      // register "/entity/latest" or "/entity". "/" alone is the root
      // pattern and stays legal.
      for (size_t i = 0; i + 1 < N; ++i)
        if (text[i] == '/' && text[i + 1] == '/')
          throw "route pattern has an empty segment";
      if (N >= 3 && text[N - 2] == '/')
        throw "route pattern has a trailing '/'";

      size_t segment = 0;
      size_t start = 0;
      while (start + 1 < N)
      {
        if (text[start] == '/')
        {
          ++start;
          continue;
        }
        size_t end = start;
        while (end + 1 < N && text[end] != '/')
          ++end;

        if (segment == max_segments)
          throw "route pattern has too many segments";
        segments_[segment] = std::string_view{text + start, end - start};

        if (text[start] == ':')
        {
          if (end - start == 1)
            throw "route pattern has an empty ':name' segment";
          ++param_count_;
        }
        else if (text[start] == '*')
        {
          // A catch-all consumes the rest of the path, so it is only
          // meaningful as the last segment — and it is one parameter.
          if (end - start == 1)
            throw "route pattern has an empty '*name' segment";
          if (end != N - 1)
            throw "route pattern has a '*name' catch-all segment that is not the last segment";
          ++param_count_;
        }
        ++segment;
        start = end;
      }
      segment_count_ = segment;
    }

    std::string_view text_;
    std::array<std::string_view, max_segments> segments_{};
    size_t segment_count_ = 0;
    size_t param_count_ = 0;
  };
} // namespace http

#endif // HTTPROUTE20_PATTERN_H
