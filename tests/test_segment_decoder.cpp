// Segment-decoder tests: with a non-identity decoder, literal edges
// retry with the decoded segment (so a plain-text pattern matches both
// wire forms), captures arrive decoded at the handler and in
// route_info, and a capture that cannot decode reports bad_path. The
// identity default keeps the raw behavior, and an escape-free request
// never calls the decoder — the raw-only fast path.
#include <doctest/doctest.h>

#include "support.h"

#include <http/segment_decoder.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace
{
  // A percent-decoder with the same contract a real one (e.g. one over
  // a URI library) has: decode into caller scratch, nullopt on a
  // malformed triplet or when scratch is too small. Counts calls so
  // tests can assert the raw-only fast path.
  struct counting_percent_decoder
  {
    inline static int calls = 0;

    static int hex_value(char c)
    {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    }

    std::optional<std::string_view> decode(std::string_view raw, char* scratch, size_t capacity) const
    {
      ++calls;
      size_t used = 0;
      for (size_t i = 0; i < raw.size(); ++i)
      {
        if (used == capacity)
          return std::nullopt;
        if (raw[i] != '%')
        {
          scratch[used++] = raw[i];
          continue;
        }
        if (i + 2 >= raw.size())
          return std::nullopt;
        const int high = hex_value(raw[i + 1]);
        const int low = hex_value(raw[i + 2]);
        if (high < 0 || low < 0)
          return std::nullopt;
        scratch[used++] = static_cast<char>(high * 16 + low);
        i += 2;
      }
      return std::string_view(scratch, used);
    }
  };

  using decoding_router =
    http::router<httptest::test_task, httptest::test_request, httptest::test_response, counting_percent_decoder>;

  // Handlers record what they were given; the tests assert on it after
  // handle() ran the coroutine eagerly to completion.
  std::string last_capture;

  httptest::test_task name_handler(httptest::test_request&, httptest::test_response&, std::string_view name)
  {
    last_capture = std::string(name);
    co_return;
  }

  httptest::test_task two_part_handler(
    httptest::test_request&,
    httptest::test_response&,
    std::string_view first,
    std::string_view second
  )
  {
    last_capture = std::string(first) + "|" + std::string(second);
    co_return;
  }

  httptest::test_task plain_handler(httptest::test_request&, httptest::test_response&)
  {
    last_capture = "ran";
    co_return;
  }

  std::string escaped_triplets(int count)
  {
    std::string raw;
    for (int i = 0; i < count; ++i)
      raw += "%41"; // 'A'
    return raw;
  }
} // namespace

TEST_CASE("a literal segment matches both wire forms of an escaped segment")
{
  decoding_router router;
  router.get("/users/josé", &plain_handler);

  CHECK(router.match("GET", "/users/josé").matched());      // raw wire form
  CHECK(router.match("GET", "/users/jos%C3%A9").matched()); // escaped wire form
}

TEST_CASE("an escaped literal under another method still reports 405")
{
  decoding_router router;
  router.post("/users/josé", &plain_handler);

  CHECK(router.match("GET", "/users/jos%C3%A9").method_not_allowed());
}

TEST_CASE("captures arrive decoded at the handler")
{
  last_capture.clear();
  decoding_router router;
  router.get("/files/:name", &name_handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/a%20b");
  CHECK(last_capture == "a b");
}

TEST_CASE("an encoded slash decodes inside the capture, not as a separator")
{
  last_capture.clear();
  decoding_router router;
  router.get("/files/:name", &name_handler);
  router.get("/files/:first/:second", &two_part_handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/a%2Fb");
  CHECK(last_capture == "a/b"); // one capture, not two segments

  // match() itself stays raw: the decoding happens at dispatch.
  auto match = router.match("GET", "/files/a%2Fb");
  REQUIRE(match.matched());
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].value == "a%2Fb");
}

TEST_CASE("middleware sees the decoded captures")
{
  last_capture.clear();
  decoding_router router;
  router.get("/files/:name", &name_handler);

  std::optional<std::string> seen;
  router.use(
    [&seen](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next, const http::route_info& info
    ) -> httptest::test_task {
      if (info.matched && info.captures.size() == 1)
        seen = std::string(info.captures[0].value);
      co_await next();
    }
  );

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/a%20b");
  CHECK(seen == std::optional<std::string>("a b"));
  CHECK(last_capture == "a b"); // the handler ran too
}

TEST_CASE("a malformed escape in a capture reports bad_path through the param-error policy")
{
  last_capture.clear();
  decoding_router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  router.get("/files/:name", &name_handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/%zz");
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
  CHECK(last_capture.empty()); // the handler never ran
}

TEST_CASE("a capture set too large for the arena reports bad_path")
{
  last_capture.clear();
  decoding_router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  router.get("/files/:name", &name_handler);

  httptest::test_request req;
  httptest::test_response res;

  // 600 decoded bytes fit the arena; 1100 do not, and that must report
  // bad_path rather than truncate a capture silently.
  router.handle(req, res, "GET", "/files/" + escaped_triplets(600));
  CHECK_FALSE(reported.has_value());
  CHECK(last_capture == std::string(600, 'A'));

  last_capture.clear();
  reported.reset();
  router.handle(req, res, "GET", "/files/" + escaped_triplets(1100));
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
  CHECK(last_capture.empty());
}

TEST_CASE("the identity decoder keeps raw captures")
{
  last_capture.clear();
  httptest::router router; // default: identity_segment_decoder
  router.get("/files/:name", &name_handler);

  auto match = router.match("GET", "/files/a%20b");
  REQUIRE(match.matched());
  CHECK(match.captures[0].value == "a%20b"); // raw, undecoded

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/a%20b");
  CHECK(last_capture == "a%20b");
}

TEST_CASE("an escape-free request never calls the decoder")
{
  counting_percent_decoder::calls = 0;
  decoding_router router;
  router.get("/users/josé", &plain_handler);
  router.get("/files/:name", &name_handler);

  CHECK(router.match("GET", "/users/josé").matched()); // raw literal hit
  CHECK_FALSE(router.match("GET", "/nope").matched()); // miss, no '%'
  CHECK(router.match("GET", "/files/abc").matched());  // param capture, no '%'
  CHECK(counting_percent_decoder::calls == 0);
}

TEST_CASE("match() with a decoder stays allocation-free")
{
  decoding_router router;
  router.get("/files/:name", &name_handler);
  router.post("/users/josé", &plain_handler);

  // Warm up lazy initialisation inside the standard library.
  (void)router.match("GET", "/files/a%20b");
  (void)router.match("DELETE", "/users/jos%C3%A9");

  long during = 0;
  {
    httptest::allocation_scope scope;
    (void)router.match("GET", "/files/a%20b");
    (void)router.match("DELETE", "/users/jos%C3%A9"); // the 405 scan decodes too
    during = scope.count();
  }
  CHECK(during == 0);
}

TEST_CASE("handle() with a decoder stays allocation-free")
{
  // arena_task's coroutine frames come from a fixed arena (support.h),
  // so the counter sees only the router's own allocations — the same
  // setup as the identity case in test_router.cpp. The terminal
  // closure must still fit move_only_fn's inline buffer, and the
  // decode arena and decoded capture_list live in handle()'s frame.
  using arena_decoding_router =
    http::router<httptest::arena_task, httptest::test_request, httptest::test_response, counting_percent_decoder>;
  arena_decoding_router router;
  router.get(
    "/files/:name",
    [](httptest::test_request&, httptest::test_response&, std::string_view) -> httptest::arena_task { co_return; }
  );

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/files/a%20b"); // warm up

  long during = 0;
  {
    httptest::allocation_scope scope;
    router.handle(req, res, "GET", "/files/a%20b");
    during = scope.count();
  }
  CHECK(during == 0);
}
