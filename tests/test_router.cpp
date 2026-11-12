// The doctest implementation (and main) lives in main.cpp; this file
// only registers test cases in the same executable.
#include <doctest/doctest.h>

#include "support.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

TEST_CASE("from_string parses segments strictly")
{
  CHECK(http::from_string<unsigned>::parse("42") == 42u);
  CHECK(http::from_string<int>::parse("-7") == -7);
  CHECK(http::from_string<std::string>::parse("hello") == "hello");
  CHECK_FALSE(http::from_string<unsigned>::parse("abc").has_value());
  CHECK_FALSE(http::from_string<unsigned>::parse("12x").has_value());
  CHECK_FALSE(http::from_string<unsigned>::parse("-1").has_value());
  CHECK_FALSE(http::from_string<unsigned>::parse("").has_value());
}

TEST_CASE("convert_captures converts in pattern order and rejects bad segments")
{
  http::capture_list captures;
  captures.push({"id", "42"});
  auto converted = http::convert_captures<std::tuple<unsigned>>(captures);
  REQUIRE(converted.has_value());
  CHECK(std::get<0>(*converted) == 42u);

  // cv-qualified elements convert too: the tuple is constructed from the
  // parsed values, not assigned into.
  auto const_converted = http::convert_captures<std::tuple<const unsigned>>(captures);
  REQUIRE(const_converted.has_value());
  CHECK(std::get<0>(*const_converted) == 42u);

  http::capture_list unparsable;
  unparsable.push({"id", "abc"});
  CHECK_FALSE(http::convert_captures<std::tuple<unsigned>>(unparsable).has_value());

  http::capture_list wrong_count;
  CHECK_FALSE(http::convert_captures<std::tuple<unsigned>>(wrong_count).has_value());

  http::capture_list none;
  CHECK(http::convert_captures<std::tuple<>>(none).has_value());
}

// A custom path-parameter type with a conforming from_string: parse
// returns std::optional<slug>, nullopt when the segment is rejected.
struct slug
{
  std::string value;
};

template<>
struct http::from_string<slug>
{
  static std::optional<slug> parse(std::string_view text)
  {
    if (text.find_first_of("0123456789") != std::string_view::npos)
      return std::nullopt;
    return slug{std::string(text)};
  }
};

// A malformed specialization: parse returns the value itself instead of
// an optional. convertible_to<std::optional<raw>> accepts raw (optional
// is constructible from it), so the loose constraint classified raw as a
// path parameter; resolve_one then failed on !value, or for a
// bool-convertible type misreported a valid segment as bad_path.
struct raw
{
  std::string value;
  explicit operator bool() const
  {
    return !value.empty();
  }
};

template<>
struct http::from_string<raw>
{
  static raw parse(std::string_view text)
  {
    return raw{std::string(text)};
  }
};

namespace
{
  httptest::test_task entity_handler(httptest::test_request&, httptest::test_response&, unsigned)
  {
    co_return;
  }

  httptest::test_task latest_handler(httptest::test_request&, httptest::test_response&)
  {
    co_return;
  }

  httptest::test_task child_handler(httptest::test_request&, httptest::test_response&, std::string, std::string)
  {
    co_return;
  }

  // noexcept handlers: registration instantiates bind_route and the
  // wrapper, so this compiles only if handler_traits handles the
  // noexcept function-pointer and lambda variants.
  httptest::test_task noexcept_handler(httptest::test_request&, httptest::test_response&, unsigned) noexcept
  {
    co_return;
  }

  auto noexcept_lambda =
    [](httptest::test_request&, httptest::test_response&, unsigned) noexcept -> httptest::test_task { co_return; };

  // A fake injected service: not a path parameter (no from_string specialization),
  // resolved from the context instead.
  struct fake_store
  {
    int value = 7;
  };

  // A per-request session, stashed by middleware and injected into handlers.
  struct session
  {
    std::string user;
  };

  // One path parameter + one injected parameter.
  httptest::test_task mixed_handler(httptest::test_request&, httptest::test_response&, unsigned, fake_store&)
  {
    co_return;
  }

  // One catch-all parameter: the rest of the path as a single string.
  // Records what it received so tests can check end-to-end resolution.
  std::optional<std::string> seen_tail;
  httptest::test_task tail_handler(httptest::test_request&, httptest::test_response&, std::string tail)
  {
    seen_tail = std::move(tail);
    co_return;
  }

  // One parameter per segment of a max_segments-long pattern, so the
  // arity check accepts a pattern with no literal segments at all.
  httptest::test_task sixteen_handler(
    httptest::test_request&,
    httptest::test_response&,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string,
    std::string
  )
  {
    co_return;
  }

  // A custom path parameter, resolved through from_string<slug>.
  // Records what it received so tests can check end-to-end resolution.
  std::optional<std::string> seen_slug;
  httptest::test_task slug_handler(httptest::test_request&, httptest::test_response&, slug s)
  {
    seen_slug = std::move(s.value);
    co_return;
  }

  // A coroutine return type awaitable through a member operator co_await
  // (the awaitable protocol's first resolution step).
  struct member_co_task
  {
    struct promise_type
    {
      member_co_task get_return_object()
      {
        return {};
      }
      std::suspend_never initial_suspend() noexcept
      {
        return {};
      }
      std::suspend_never final_suspend() noexcept
      {
        return {};
      }
      void return_void()
      {
      }
      void unhandled_exception()
      {
        std::terminate();
      }
    };

    struct awaiter
    {
      bool await_ready() const noexcept
      {
        return true;
      }
      void await_suspend(std::coroutine_handle<>) noexcept
      {
      }
      void await_resume() noexcept
      {
      }
    };

    awaiter operator co_await()
    {
      return {};
    }
  };

  // A coroutine return type awaitable through a free operator co_await
  // (found by ADL — the protocol's second resolution step).
  struct free_co_task
  {
    struct promise_type
    {
      free_co_task get_return_object()
      {
        return {};
      }
      std::suspend_never initial_suspend() noexcept
      {
        return {};
      }
      std::suspend_never final_suspend() noexcept
      {
        return {};
      }
      void return_void()
      {
      }
      void unhandled_exception()
      {
        std::terminate();
      }
    };
  };

  struct free_co_awaiter
  {
    bool await_ready() const noexcept
    {
      return true;
    }
    void await_suspend(std::coroutine_handle<>) noexcept
    {
    }
    void await_resume() noexcept
    {
    }
  };

  free_co_awaiter operator co_await(free_co_task)
  {
    return {};
  }

  // An awaiter bound to one specific promise type: await_suspend only
  // accepts that promise's handle, so the type is not freely awaitable
  // in any promise and must stay rejected.
  struct bound_task
  {
    struct promise_type
    {
      bound_task get_return_object()
      {
        return {};
      }
      std::suspend_never initial_suspend() noexcept
      {
        return {};
      }
      std::suspend_never final_suspend() noexcept
      {
        return {};
      }
      void return_void()
      {
      }
      void unhandled_exception()
      {
        std::terminate();
      }
    };

    struct bound_awaiter
    {
      bool await_ready() const noexcept
      {
        return true;
      }
      void await_suspend(std::coroutine_handle<promise_type>) noexcept
      {
      }
      void await_resume() noexcept
      {
      }
    };

    bound_awaiter operator co_await()
    {
      return {};
    }
  };

  // await_suspend returning something other than void/bool/handle makes
  // co_await ill-formed, so the type must stay rejected.
  struct bad_suspend_task
  {
    struct promise_type
    {
      bad_suspend_task get_return_object()
      {
        return {};
      }
      std::suspend_never initial_suspend() noexcept
      {
        return {};
      }
      std::suspend_never final_suspend() noexcept
      {
        return {};
      }
      void return_void()
      {
      }
      void unhandled_exception()
      {
        std::terminate();
      }
    };

    struct awaiter
    {
      bool await_ready() const noexcept
      {
        return true;
      }
      int await_suspend(std::coroutine_handle<>) noexcept
      {
        return 0;
      }
      void await_resume() noexcept
      {
      }
    };

    awaiter operator co_await()
    {
      return {};
    }
  };

  // An explicitly injected std::string — opt-in to context injection for
  // a type that also has a from_string specialization. Records what it
  // received so tests can check end-to-end resolution.
  std::optional<std::string> seen_config;
  httptest::test_task config_handler(
    httptest::test_request&,
    httptest::test_response&,
    http::injected<std::string> config
  )
  {
    seen_config = config.value;
    co_return;
  }

  // One path parameter plus one explicitly injected std::string.
  std::optional<unsigned> seen_mixed_id;
  std::optional<std::string> seen_mixed_config;
  httptest::test_task mixed_config_handler(
    httptest::test_request&,
    httptest::test_response&,
    unsigned id,
    http::injected<std::string> config
  )
  {
    seen_mixed_id = id;
    seen_mixed_config = config.value;
    co_return;
  };

  // A response double with only the write contract (setters, no getters):
  // the router forwards Res opaquely, so this must satisfy response_like
  // even without status()/header(name) getters.
  struct write_only_response
  {
    int status_code = 200;
    write_only_response& status(int code)
    {
      status_code = code;
      return *this;
    }
    write_only_response& header(std::string, std::string)
    {
      return *this;
    }
  };
} // namespace

TEST_CASE("typed routes match and capture parameters")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);

  auto match = router.match("GET", "/entity/7");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/entity/:id");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].name == "id");
  CHECK(match.captures[0].value == "7");

  CHECK_FALSE(router.match("GET", "/entity/7/extra").matched());
  CHECK_FALSE(router.match("POST", "/entity/7").matched());
}

TEST_CASE("a literal route wins over a parameterised one regardless of registration order")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler); // registered first
  router.get("/entity/latest", &latest_handler);

  auto literal = router.match("GET", "/entity/latest");
  REQUIRE(literal.matched());
  CHECK(literal.pattern == "/entity/latest");
  CHECK(literal.captures.size() == 0);

  auto parameterised = router.match("GET", "/entity/42");
  REQUIRE(parameterised.matched());
  CHECK(parameterised.pattern == "/entity/:id");
  REQUIRE(parameterised.captures.size() == 1);
  CHECK(parameterised.captures[0].name == "id");
  CHECK(parameterised.captures[0].value == "42");
}

TEST_CASE("matching backtracks from a literal branch to a parameter branch")
{
  httptest::router router;
  router.get("/entity/latest", &latest_handler);
  router.get("/entity/:id/children/:child", &child_handler);

  // "latest" is a literal edge, but it has no "children" child, so the
  // walk falls back to the ":id" edge and captures "latest" as the id.
  auto match = router.match("GET", "/entity/latest/children/7");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/entity/:id/children/:child");
  REQUIRE(match.captures.size() == 2);
  CHECK(match.captures[0].name == "id");
  CHECK(match.captures[0].value == "latest");
  CHECK(match.captures[1].name == "child");
  CHECK(match.captures[1].value == "7");
}

TEST_CASE("noexcept handlers register on the router")
{
  httptest::router router;
  router.get("/entity/:id", &noexcept_handler);
  router.get("/lambda/:id", noexcept_lambda);

  CHECK(router.match("GET", "/entity/1").matched());
  CHECK(router.match("GET", "/lambda/1").matched());
}

TEST_CASE("route(method, ...) registers any method; the verb aliases delegate to it")
{
  httptest::router router;
  router.route("GET", "/entity/:id", &entity_handler);
  router.route("PURGE", "/entity/:id", &entity_handler);
  router.post("/entity/:id", &entity_handler);
  router.put("/entity/:id", &entity_handler);
  router.patch("/entity/:id", &entity_handler);
  router.del("/entity/:id", &entity_handler);
  router.head("/entity/:id", &entity_handler);
  router.options("/entity/:id", &entity_handler);

  // Every method has its own trie, and each alias lands in its method's.
  for (std::string_view method : {"GET", "PURGE", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"})
  {
    auto match = router.match(method, "/entity/7");
    REQUIRE(match.matched());
    CHECK(match.pattern == "/entity/:id");
    REQUIRE(match.captures.size() == 1);
    CHECK(match.captures[0].name == "id");
    CHECK(match.captures[0].value == "7");
  }

  // Unregistered methods do not match, and methods are case-sensitive.
  CHECK_FALSE(router.match("TRACE", "/entity/7").matched());
  CHECK_FALSE(router.match("get", "/entity/7").matched());

  // route() and the aliases share one trie per method.
  router.get("/same", &latest_handler);
  router.route("GET", "/same-too", &latest_handler);
  CHECK(router.match("GET", "/same").matched());
  CHECK(router.match("GET", "/same-too").matched());
  CHECK_FALSE(router.match("POST", "/same").matched());
}

TEST_CASE("the template-argument pattern syntax get<\"...\"> registers and dispatches like the literal syntax")
{
  httptest::router router;
  router.get<"/entity/:id">(&entity_handler);
  router.route<"/same">("PURGE", &latest_handler);

  auto match = router.match("GET", "/entity/7");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/entity/:id");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].name == "id");
  CHECK(match.captures[0].value == "7");

  auto purged = router.match("PURGE", "/same");
  REQUIRE(purged.matched());
  CHECK(purged.pattern == "/same");

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7");
  CHECK(res.status() == 200);
}

TEST_CASE(
  "HEAD and OPTIONS derivation is the consumer's: head() delegates to the GET handler, the 405 handler answers OPTIONS"
)
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);
  // Same handler under HEAD: the router dispatches HEAD like any other
  // method; suppressing the body is the transport's job (RFC 9110
  // 9.3.2), not the router's — Res is opaque beyond status/header.
  router.head("/entity/:id", &entity_handler);

  router.set_method_not_allowed(
    [](
      httptest::test_request& req, httptest::test_response& res, std::span<const std::string_view> allowed
    ) -> httptest::test_task {
      if (req.method == "OPTIONS")
      {
        std::string allow = "OPTIONS";
        for (std::string_view method : allowed)
        {
          allow += ", ";
          allow += method;
        }
        res.status(204);
        res.header("Allow", allow);
      }
      else
      {
        res.status(405);
      }
      co_return;
    }
  );

  httptest::test_request req;
  httptest::test_response res;

  // HEAD dispatches to the handler registered under HEAD.
  router.handle(req, res, "HEAD", "/entity/7");
  CHECK(res.status() == 200);

  // OPTIONS on a known path is answered 204 + Allow from the list.
  res = httptest::test_response{};
  req.method = "OPTIONS";
  router.handle(req, res, "OPTIONS", "/entity/7");
  CHECK(res.status() == 204);
  REQUIRE(res.header("Allow").has_value());
  CHECK(res.header("Allow").value() == "OPTIONS, GET, HEAD");

  // Any other wrong method still gets the plain 405.
  res = httptest::test_response{};
  req.method = "DELETE";
  router.handle(req, res, "DELETE", "/entity/7");
  CHECK(res.status() == 405);
}

TEST_CASE("a wrong-method request is reported as 405 with the sorted allowed methods")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);
  router.post("/entity/:id", &entity_handler);
  router.del("/other", &latest_handler);

  auto wrong = router.match("DELETE", "/entity/7");
  CHECK_FALSE(wrong.matched());
  REQUIRE(wrong.method_not_allowed());
  REQUIRE(wrong.allowed().size() == 2);
  CHECK(wrong.allowed()[0] == "GET");
  CHECK(wrong.allowed()[1] == "POST");

  // An unknown path is a plain 404: no method matches it.
  auto unknown = router.match("DELETE", "/nope");
  CHECK_FALSE(unknown.matched());
  CHECK_FALSE(unknown.method_not_allowed());
  CHECK(unknown.allowed().empty());

  // A matched request reports neither.
  auto ok = router.match("GET", "/entity/7");
  REQUIRE(ok.matched());
  CHECK_FALSE(ok.method_not_allowed());
  CHECK(ok.allowed().empty());
}

TEST_CASE("a trailing *name catch-all captures the rest of the path, slashes included")
{
  httptest::router router;
  router.get("/static/*path", &tail_handler);

  auto match = router.match("GET", "/static/css/site.css");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/static/*path");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].name == "path");
  CHECK(match.captures[0].value == "css/site.css");

  auto single = router.match("GET", "/static/logo.png");
  REQUIRE(single.matched());
  CHECK(single.captures[0].value == "logo.png");

  // A catch-all needs at least one segment: "/static" alone does not match.
  CHECK_FALSE(router.match("GET", "/static").matched());
  CHECK_FALSE(router.match("GET", "/static/").matched());
  CHECK_FALSE(router.match("GET", "/other/x").matched());

  // A root-level catch-all matches any non-empty path.
  httptest::router root;
  root.get("/*all", &tail_handler);
  auto any = root.match("GET", "/a/b/c");
  REQUIRE(any.matched());
  CHECK(any.captures[0].name == "all");
  CHECK(any.captures[0].value == "a/b/c");
  CHECK_FALSE(root.match("GET", "/").matched());
}

TEST_CASE("the catch-all capture resolves into the handler's parameter")
{
  httptest::router router;
  router.get("/static/*path", &tail_handler);

  httptest::test_request req;
  httptest::test_response res;
  auto match = router.match("GET", "/static/js/app/main.js");
  REQUIRE(match.matched());
  seen_tail.reset();
  (*match.handler)(req, res, match.captures);
  REQUIRE(seen_tail.has_value());
  CHECK(*seen_tail == "js/app/main.js");
}

TEST_CASE("literal and :name routes win over a catch-all, which backtracks to them")
{
  httptest::router router;
  router.get("/files/*path", &tail_handler);
  router.get("/files/readme", &latest_handler); // literal beats catch-all
  router.get("/dl/:id", &entity_handler);       // param beats catch-all on one segment
  router.get("/dl/*path", &tail_handler);

  auto literal = router.match("GET", "/files/readme");
  REQUIRE(literal.matched());
  CHECK(literal.pattern == "/files/readme");
  CHECK(literal.captures.size() == 0);

  auto tail = router.match("GET", "/files/a/b/c");
  REQUIRE(tail.matched());
  CHECK(tail.pattern == "/files/*path");
  CHECK(tail.captures[0].value == "a/b/c");

  auto param = router.match("GET", "/dl/7");
  REQUIRE(param.matched());
  CHECK(param.pattern == "/dl/:id");
  CHECK(param.captures[0].value == "7");

  // The param branch fails on a second segment, so the walk backtracks
  // to the catch-all.
  auto deep = router.match("GET", "/dl/7/extra");
  REQUIRE(deep.matched());
  CHECK(deep.pattern == "/dl/*path");
  CHECK(deep.captures[0].value == "7/extra");
}

// --- path length limit ---

TEST_CASE("a path longer than max_segments does not match a literal pattern")
{
  httptest::router router;
  router.get("/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p", &latest_handler);

  // Exactly max_segments (16) segments: the pattern is exhausted and the
  // path is exhausted together, so this is a match.
  CHECK(router.match("GET", "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p").matched());

  // One segment more than the pattern: there is no segment left in the
  // pattern for it, so this must not match.
  CHECK_FALSE(router.match("GET", "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q").matched());
}

TEST_CASE("a path longer than max_segments does not match a parameterised pattern")
{
  httptest::router router;
  router.get("/:a/:b/:c/:d/:e/:f/:g/:h/:i/:j/:k/:l/:m/:n/:o/:p", &sixteen_handler);

  CHECK(router.match("GET", "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16").matched());
  CHECK_FALSE(router.match("GET", "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17").matched());
}

TEST_CASE("a catch-all matches a path longer than max_segments")
{
  httptest::router router;
  router.get("/static/*path", &tail_handler);

  // The catch-all captures the raw remainder of the path from its own
  // offset, so it does not depend on the segment list holding every
  // segment. Depth past max_segments is therefore irrelevant to it.
  auto match = router.match("GET", "/static/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/static/*path");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].value == "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q");
}

// --- percent-encoding ---

TEST_CASE("the router does not percent-decode: captures are the raw path bytes")
{
  // Decoding is the server's job, done before match()/handle() is called
  // (README "Matching semantics"). The router matches and captures the
  // path exactly as given, so an encoded slash never becomes a segment
  // boundary here: "/u/a%2Fb" captures "a%2Fb".
  httptest::router router;
  router.get("/u/:name", &slug_handler);

  auto encoded = router.match("GET", "/u/a%2Fb");
  REQUIRE(encoded.matched());
  REQUIRE(encoded.captures.size() == 1);
  CHECK(encoded.captures[0].value == "a%2Fb");

  // Literal segments match the raw text too: "/a b" does not match
  // "/a%20b" — a server that decodes first would pass "/a b" instead.
  httptest::router literals;
  literals.get("/a b", &latest_handler);
  CHECK_FALSE(literals.match("GET", "/a%20b").matched());
  CHECK(literals.match("GET", "/a b").matched());
}

// --- context and injected parameters ---

static_assert(http::path_parameter<unsigned>);
static_assert(http::path_parameter<std::string>);
static_assert(http::path_parameter<std::string_view>);
static_assert(!http::path_parameter<fake_store>);
static_assert(!http::path_parameter<bool>);
static_assert(http::path_parameter<slug>);
static_assert(!http::path_parameter<raw>);
static_assert(http::injectable<fake_store>);
static_assert(http::route_param<unsigned>);
static_assert(http::route_param<fake_store>);
static_assert(http::detail::path_param_count_v<decltype(&entity_handler)> == 1);
static_assert(http::detail::path_param_count_v<decltype(&latest_handler)> == 0);
static_assert(http::detail::path_param_count_v<decltype(&child_handler)> == 2);
static_assert(http::detail::path_param_count_v<decltype(&mixed_handler)> == 1);
static_assert(http::route_param<http::injected<std::string>>);
static_assert(http::detail::path_param_count_v<decltype(&config_handler)> == 0);
static_assert(http::detail::path_param_count_v<decltype(&mixed_config_handler)> == 1);
static_assert(http::response_like<httptest::test_response>);
static_assert(http::response_readable<httptest::test_response>);
static_assert(http::response_like<write_only_response>);
static_assert(!http::response_readable<write_only_response>);

TEST_CASE("a custom from_string specialization parses a path parameter end to end")
{
  httptest::router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  router.get("/u/:name", &slug_handler);

  auto match = router.match("GET", "/u/hello");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/u/:name");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].value == "hello");

  httptest::test_request req;
  httptest::test_response res;
  seen_slug.reset();
  (*match.handler)(req, res, match.captures);
  REQUIRE(seen_slug.has_value());
  CHECK(*seen_slug == "hello");
  CHECK_FALSE(reported.has_value());

  // A segment the specialization rejects is a bad_path, reported through
  // the error policy, not a crash and not a silent success.
  reported.reset();
  seen_slug.reset();
  auto bad = router.match("GET", "/u/abc123");
  REQUIRE(bad.matched());
  (*bad.handler)(req, res, bad.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
  CHECK_FALSE(seen_slug.has_value());
}

TEST_CASE("context resolves entries through the parent chain and allows shadowing")
{
  auto parent = std::make_shared<http::context>();
  parent->set<fake_store>(std::make_shared<fake_store>());
  parent->set<int>(std::make_shared<int>(5));

  auto child = std::make_shared<http::context>(parent);
  CHECK(child->parent().get() == parent.get());
  CHECK(child->get<fake_store>() == parent->get<fake_store>());
  REQUIRE(child->get<int>() != nullptr);
  CHECK(*child->get<int>() == 5);
  CHECK(child->get<double>() == nullptr);

  // A child entry shadows the parent's; the parent is unaffected.
  child->set<fake_store>(std::make_shared<fake_store>(fake_store{42}));
  CHECK(child->get<fake_store>()->value == 42);
  CHECK(parent->get<fake_store>()->value == 7);
}

TEST_CASE("resolve_params parses path params and injects from the context")
{
  auto ctx = std::make_shared<http::context>();
  auto store = std::make_shared<fake_store>();
  ctx->set<fake_store>(store);

  http::capture_list captures;
  captures.push({"id", "42"});

  auto resolved = http::resolve_params<std::tuple<unsigned, fake_store&>>(captures, ctx.get());
  REQUIRE(resolved.error == http::param_error::none);
  REQUIRE(resolved.values.has_value());
  CHECK(std::get<0>(*resolved.values) == 42u);
  CHECK(&std::get<1>(*resolved.values) == store.get());
}

TEST_CASE("resolve_params reports bad path segments and missing injections")
{
  auto ctx = std::make_shared<http::context>();

  http::capture_list bad;
  bad.push({"id", "abc"});
  auto bad_path = http::resolve_params<std::tuple<unsigned>>(bad, ctx.get());
  CHECK(bad_path.error == http::param_error::bad_path);
  CHECK_FALSE(bad_path.values.has_value());

  http::capture_list ok;
  ok.push({"id", "7"});
  auto missing = http::resolve_params<std::tuple<unsigned, fake_store&>>(ok, ctx.get());
  CHECK(missing.error == http::param_error::missing_injection);
  CHECK_FALSE(missing.values.has_value());

  // A null context: path params still parse, injections fail.
  auto path_only = http::resolve_params<std::tuple<unsigned>>(ok, nullptr);
  REQUIRE(path_only.error == http::param_error::none);
  CHECK(std::get<0>(*path_only.values) == 7u);
  auto no_ctx = http::resolve_params<std::tuple<unsigned, fake_store&>>(ok, nullptr);
  CHECK(no_ctx.error == http::param_error::missing_injection);

  // A bad path segment outranks a missing injection, in either parameter
  // order: the client error is reported, not the config error.
  auto both = http::resolve_params<std::tuple<unsigned, fake_store&>>(bad, nullptr);
  CHECK(both.error == http::param_error::bad_path);
  auto both_reversed = http::resolve_params<std::tuple<fake_store&, unsigned>>(bad, nullptr);
  CHECK(both_reversed.error == http::param_error::bad_path);
}

TEST_CASE("router hands out per-request child contexts of the app context")
{
  httptest::router router;
  auto app = std::make_shared<http::context>();
  auto store = std::make_shared<fake_store>();
  app->set<fake_store>(store);
  router.set_context(app);
  CHECK(router.context().get() == app.get());

  auto request_ctx = router.new_request_context();
  CHECK(request_ctx->parent().get() == app.get());
  CHECK(request_ctx->get<fake_store>() == store.get());

  // Per-request entries do not leak into the app context.
  request_ctx->set<int>(std::make_shared<int>(1));
  CHECK(app->get<int>() == nullptr);
}

TEST_CASE("routes with injected parameters register and match")
{
  httptest::router router;
  router.get("/entity/:id", &mixed_handler);

  auto match = router.match("GET", "/entity/9");
  REQUIRE(match.matched());
  CHECK(match.pattern == "/entity/:id");
  REQUIRE(match.captures.size() == 1);
  CHECK(match.captures[0].value == "9");
}

// --- error policy ---

TEST_CASE("bind_route reports resolution failures through the error callback")
{
  httptest::router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  router.get("/entity/:id", &entity_handler);
  router.get("/mixed/:id", &mixed_handler);

  // A segment that does not parse: bad_path.
  httptest::test_request req;
  httptest::test_response res;
  auto bad = router.match("GET", "/entity/abc");
  REQUIRE(bad.matched());
  (*bad.handler)(req, res, bad.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);

  // An injected parameter missing from the context: missing_injection.
  reported.reset();
  auto missing = router.match("GET", "/mixed/7");
  REQUIRE(missing.matched());
  (*missing.handler)(req, res, missing.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::missing_injection);

  // A successful resolution never calls the callback.
  reported.reset();
  auto ok = router.match("GET", "/entity/7");
  REQUIRE(ok.matched());
  (*ok.handler)(req, res, ok.captures);
  CHECK_FALSE(reported.has_value());
}

TEST_CASE("without an error callback a failed resolution produces no response")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);

  auto match = router.match("GET", "/entity/abc");
  REQUIRE(match.matched());
  httptest::test_request req;
  httptest::test_response res;
  (*match.handler)(req, res, match.captures); // must not crash
  CHECK(res.status_code == 200);              // and nothing was written
}

TEST_CASE("an error policy set after registration still applies to earlier routes")
{
  // The policy is looked up at request time, not captured at registration
  // time. That is the documented behaviour, and it is why the bound
  // handler holds a handle to the policy rather than a copy of it.
  httptest::router router;
  router.get("/entity/:id", &entity_handler); // registered first

  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );

  httptest::test_request req;
  httptest::test_response res;
  auto match = router.match("GET", "/entity/abc");
  REQUIRE(match.matched());
  (*match.handler)(req, res, match.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
}

// --- middleware pipeline ---

TEST_CASE("the middleware pipeline wraps the matched handler in onion order")
{
  httptest::router router;
  std::vector<std::string> log;
  router.use(
    [&log](httptest::test_request&, httptest::test_response&, httptest::router::next_fn next) -> httptest::test_task {
      log.push_back("enter:first");
      co_await next();
      log.push_back("exit:first");
    }
  );
  router.use(
    [&log](httptest::test_request&, httptest::test_response&, httptest::router::next_fn next) -> httptest::test_task {
      log.push_back("enter:second");
      co_await next();
      log.push_back("exit:second");
    }
  );
  auto handler = [&log](httptest::test_request&, httptest::test_response&, unsigned) -> httptest::test_task {
    log.push_back("handler");
    co_return;
  };
  router.get("/entity/:id", handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7");

  CHECK(log == std::vector<std::string>{"enter:first", "enter:second", "handler", "exit:second", "exit:first"});
}

TEST_CASE("a middleware can short-circuit by not calling next")
{
  httptest::router router;
  bool handler_ran = false;
  router.use(
    [](httptest::test_request&, httptest::test_response& res, httptest::router::next_fn) -> httptest::test_task {
      res.status(401);
      res.header("www-authenticate", "Bearer");
      co_return; // next is never called
    }
  );
  auto handler = [&handler_ran](httptest::test_request&, httptest::test_response&, unsigned) -> httptest::test_task {
    handler_ran = true;
    co_return;
  };
  router.get("/entity/:id", handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7");
  CHECK_FALSE(handler_ran);
  CHECK(res.status() == 401);
  CHECK(res.header("www-authenticate") == std::optional<std::string_view>{"Bearer"});
}

TEST_CASE("post-processing middleware inspects the response through the getters")
{
  httptest::router router;
  std::optional<int> seen_status;
  std::optional<std::string_view> seen_content_type;
  router.use(
    [&](httptest::test_request&, httptest::test_response& res, httptest::router::next_fn next) -> httptest::test_task {
      co_await next();
      seen_status = res.status();
      seen_content_type = res.header("content-type");
    }
  );
  auto handler = [](httptest::test_request&, httptest::test_response& res) -> httptest::test_task {
    res.status(201);
    res.header("content-type", "application/json");
    co_return;
  };
  router.get("/entity", handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity");
  CHECK(seen_status == 201);
  CHECK(seen_content_type == std::optional<std::string_view>{"application/json"});
}

TEST_CASE("middleware and handlers share per-request state through the context")
{
  httptest::router router;
  router.use(
    [](httptest::test_request& req, httptest::test_response&, httptest::router::next_fn next) -> httptest::test_task {
      req.ctx->set<session>(std::make_shared<session>(session{"alice"}));
      co_await next();
    }
  );
  std::optional<std::string> seen_user;
  auto handler = [&seen_user](httptest::test_request&, httptest::test_response&, session& s) -> httptest::test_task {
    seen_user = s.user;
    co_return;
  };
  router.get("/me", handler);

  auto ctx = std::make_shared<http::context>();
  httptest::test_request req{ctx.get()};
  httptest::test_response res;
  router.handle(req, res, "GET", "/me");
  REQUIRE(seen_user.has_value());
  CHECK(*seen_user == "alice");
}

TEST_CASE("the documented req.context()->set pattern works through a const context")
{
  // A request exposing only const context* — exactly the type
  // request_like guarantees — so the README's req.context()->set<T>()
  // middleware pattern must compile and run against it.
  struct const_context_request
  {
    const http::context* ctx = nullptr;
    const http::context* context() const
    {
      return ctx;
    }
  };
  static_assert(http::request_like<const_context_request>);

  using const_router = http::router<httptest::test_task, const_context_request, httptest::test_response>;
  const_router router;
  router.use(
    [](const_context_request& req, httptest::test_response&, const_router::next_fn next) -> httptest::test_task {
      req.context()->set<session>(std::make_shared<session>(session{"alice"}));
      co_await next();
    }
  );
  std::optional<std::string> seen_user;
  auto handler = [&seen_user](const_context_request&, httptest::test_response&, session& s) -> httptest::test_task {
    seen_user = s.user;
    co_return;
  };
  router.get("/me", handler);

  auto ctx = std::make_shared<http::context>();
  const_context_request req{ctx.get()};
  httptest::test_response res;
  router.handle(req, res, "GET", "/me");
  REQUIRE(seen_user.has_value());
  CHECK(*seen_user == "alice");
}

TEST_CASE("move-only handlers and middleware register and run")
{
  httptest::router router;
  std::optional<int> seen;
  auto owned = std::make_unique<int>(41);
  auto move_only_handler = [owned = std::move(owned), &seen](
                             httptest::test_request&, httptest::test_response&, unsigned id
                           ) -> httptest::test_task {
    seen = *owned + static_cast<int>(id);
    co_return;
  };
  static_assert(!std::is_copy_constructible_v<decltype(move_only_handler)>);
  static_assert(
    http::detail::request_response_handler<decltype(move_only_handler), httptest::test_request, httptest::test_response>
  );
  router.get("/entity/:id", std::move(move_only_handler));

  int middleware_calls = 0;
  auto mw_owned = std::make_unique<int>(0);
  auto move_only_mw = [mw_owned = std::move(mw_owned), &middleware_calls](
                        httptest::test_request&, httptest::test_response&, httptest::router::next_fn next
                      ) -> httptest::test_task {
    ++*mw_owned;
    ++middleware_calls;
    co_await next();
  };
  static_assert(!std::is_copy_constructible_v<decltype(move_only_mw)>);
  static_assert(
    http::middleware_like<decltype(move_only_mw), httptest::test_task, httptest::test_request, httptest::test_response>
  );
  router.use(std::move(move_only_mw));

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/1");
  REQUIRE(seen.has_value());
  CHECK(*seen == 42);
  CHECK(middleware_calls == 1);
}

TEST_CASE("handle() runs the not-found handler inside the pipeline when nothing matches")
{
  httptest::router router;
  bool middleware_ran = false;
  router.use(
    [&middleware_ran](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next
    ) -> httptest::test_task {
      middleware_ran = true;
      co_await next();
    }
  );
  bool not_found_ran = false;
  router.set_not_found([&not_found_ran](httptest::test_request&, httptest::test_response& res) -> httptest::test_task {
    not_found_ran = true;
    res.status(404);
    co_return;
  });
  router.get("/entity/:id", &entity_handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/nope");
  CHECK(middleware_ran);
  CHECK(not_found_ran);
  CHECK(res.status() == 404);
}

TEST_CASE("handle() dispatches a wrong-method request to the 405 handler with the Allow list")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);
  router.post("/entity/:id", &entity_handler);

  bool middleware_ran = false;
  router.use(
    [&middleware_ran](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next
    ) -> httptest::test_task {
      middleware_ran = true;
      co_await next();
    }
  );

  bool method_not_allowed_ran = false;
  router.set_method_not_allowed(
    [&method_not_allowed_ran](
      httptest::test_request&, httptest::test_response& res, std::span<const std::string_view> allowed
    ) -> httptest::test_task {
      method_not_allowed_ran = true;
      res.status(405);
      std::string allow;
      for (std::string_view method : allowed)
      {
        if (!allow.empty())
          allow += ", ";
        allow += method;
      }
      res.header("Allow", allow);
      co_return;
    }
  );

  bool not_found_ran = false;
  router.set_not_found([&not_found_ran](httptest::test_request&, httptest::test_response& res) -> httptest::test_task {
    not_found_ran = true;
    res.status(404);
    co_return;
  });

  httptest::test_request req;
  httptest::test_response res;

  // Wrong method on a known path: 405 with the Allow header, and the
  // middleware onion observes it exactly like a 404.
  router.handle(req, res, "DELETE", "/entity/7");
  CHECK(middleware_ran);
  CHECK(method_not_allowed_ran);
  CHECK_FALSE(not_found_ran);
  CHECK(res.status() == 405);
  REQUIRE(res.header("Allow").has_value());
  CHECK(res.header("Allow").value() == "GET, POST");

  // Unknown path: still the not-found handler.
  res = httptest::test_response{};
  method_not_allowed_ran = false;
  middleware_ran = false;
  router.handle(req, res, "DELETE", "/nope");
  CHECK(middleware_ran);
  CHECK_FALSE(method_not_allowed_ran);
  CHECK(not_found_ran);
  CHECK(res.status() == 404);

  // A matched request is unaffected.
  res = httptest::test_response{};
  method_not_allowed_ran = false;
  not_found_ran = false;
  router.handle(req, res, "GET", "/entity/7");
  CHECK_FALSE(method_not_allowed_ran);
  CHECK_FALSE(not_found_ran);
  CHECK(res.status() == 200);
}

TEST_CASE("middleware receives the matched route as a fourth argument")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);

  std::optional<std::string> seen_pattern;
  std::optional<std::string> seen_capture;
  bool seen_matched = false;
  router.use(
    [&](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next, const http::route_info& info
    ) -> httptest::test_task {
      // The views are valid only for the duration of the dispatch, so copy
      // what the test checks.
      seen_pattern = std::string(info.pattern);
      seen_matched = info.matched;
      if (!info.captures.empty())
        seen_capture = std::string(info.captures[0].value);
      co_await next();
    }
  );

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7");
  REQUIRE(seen_pattern.has_value());
  CHECK(*seen_pattern == "/entity/:id");
  CHECK(seen_matched);
  REQUIRE(seen_capture.has_value());
  CHECK(*seen_capture == "7");
}

TEST_CASE("middleware sees 405 and 404 through the route_info argument")
{
  httptest::router router;
  router.get("/entity/:id", &entity_handler);

  bool seen_matched = true;
  bool seen_405 = false;
  size_t seen_allowed = 0;
  std::optional<std::string> seen_first_allowed;
  std::optional<std::string> seen_pattern;
  router.use(
    [&](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next, const http::route_info& info
    ) -> httptest::test_task {
      seen_matched = info.matched;
      seen_405 = info.method_not_allowed;
      seen_allowed = info.allowed_methods.size();
      if (!info.allowed_methods.empty())
        seen_first_allowed = std::string(info.allowed_methods[0]);
      seen_pattern = std::string(info.pattern);
      co_await next();
    }
  );

  httptest::test_request req;
  httptest::test_response res;

  // Wrong method on a known path: the middleware sees the 405 and the
  // allowed methods.
  router.handle(req, res, "DELETE", "/entity/7");
  CHECK_FALSE(seen_matched);
  CHECK(seen_405);
  CHECK(seen_allowed == 1);
  REQUIRE(seen_first_allowed.has_value());
  CHECK(*seen_first_allowed == "GET");

  // Unknown path: a plain 404, empty pattern.
  router.handle(req, res, "DELETE", "/nope");
  CHECK_FALSE(seen_matched);
  CHECK_FALSE(seen_405);
  CHECK(seen_allowed == 0);
  REQUIRE(seen_pattern.has_value());
  CHECK(seen_pattern->empty());
}

TEST_CASE("handle() reports parameter-resolution failures through the error policy inside the pipeline")
{
  httptest::router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  bool middleware_ran = false;
  router.use(
    [&middleware_ran](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next
    ) -> httptest::test_task {
      middleware_ran = true;
      co_await next();
    }
  );
  router.get("/entity/:id", &entity_handler);

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/abc");
  CHECK(middleware_ran);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
}

TEST_CASE("per-route middleware as a decorator: auth on some routes only (documented pattern)")
{
  // A decorator wraps one concrete handler before registration; routes
  // registered without it stay public. No router support needed — this
  // is the usage pattern for per-route middleware.
  auto require_session =
    [](std::function<httptest::test_task(httptest::test_request&, httptest::test_response&, unsigned)> next) {
      return [next](httptest::test_request& req, httptest::test_response& res, unsigned id) -> httptest::test_task {
        if (req.ctx == nullptr || req.ctx->get<session>() == nullptr)
        {
          res.status(401);
          co_return;
        }
        co_await next(req, res, id);
      };
    };

  httptest::router router;
  bool admin_ran = false;
  auto admin = [&admin_ran](httptest::test_request&, httptest::test_response&, unsigned) -> httptest::test_task {
    admin_ran = true;
    co_return;
  };
  bool public_ran = false;
  auto pub = [&public_ran](httptest::test_request&, httptest::test_response&) -> httptest::test_task {
    public_ran = true;
    co_return;
  };
  router.get("/admin/:id", require_session(admin));
  router.get("/public", pub);

  // No session in the request context: the wrapped route short-circuits.
  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/admin/1");
  CHECK_FALSE(admin_ran);
  CHECK(res.status() == 401);

  // The unwrapped route stays public.
  res = httptest::test_response{};
  router.handle(req, res, "GET", "/public");
  CHECK(public_ran);

  // With a session in the request context the wrapped route runs.
  auto ctx = std::make_shared<http::context>();
  ctx->set<session>(std::make_shared<session>(session{"alice"}));
  req.ctx = ctx.get();
  res = httptest::test_response{};
  router.handle(req, res, "GET", "/admin/1");
  CHECK(admin_ran);
}

// --- allocation behaviour ---

TEST_CASE("match() performs no heap allocation")
{
  // match() is documented as allocation-free: captures are a fixed-size
  // array and the trie walk needs no heap. It also copies the bound
  // handler into the route_match, so this pins the property that keeps
  // that copy free: the closure is small enough for libc++ and MSVC, and
  // trivially copyable for libstdc++, which stores a closure inline only
  // in that case. Growing the closure, or giving it a non-trivial member
  // such as a shared_ptr, turns every match() into a heap allocation
  // without failing any functional test.
  httptest::router router;
  router.get("/entity/:id", &entity_handler);

  // Warm up: a first call may trigger lazy initialisation inside the
  // standard library, which is not what this test measures.
  auto warm = router.match("GET", "/entity/7");
  REQUIRE(warm.matched());

  long during = 0;
  bool matched = false;
  {
    httptest::allocation_scope scope;
    auto match = router.match("GET", "/entity/7");
    during = scope.count();
    matched = match.matched();
  }

  CHECK(matched);
  CHECK(during == 0);
}

TEST_CASE("match() on a miss — 404 and 405 alike — performs no heap allocation")
{
  // The 405 scan walks the other methods' tries on a miss; it must stay
  // allocation-free like the matched path.
  httptest::router router;
  router.get("/entity/:id", &entity_handler);
  router.post("/entity/:id", &entity_handler);

  // Warm up lazy initialisation.
  CHECK(router.match("DELETE", "/entity/7").method_not_allowed());
  CHECK_FALSE(router.match("DELETE", "/nope").method_not_allowed());

  long during_405 = 0;
  bool is_405 = false;
  {
    httptest::allocation_scope scope;
    auto m = router.match("DELETE", "/entity/7");
    during_405 = scope.count();
    is_405 = m.method_not_allowed();
  }
  CHECK(is_405);
  CHECK(during_405 == 0);

  long during_404 = 0;
  bool is_404 = false;
  {
    httptest::allocation_scope scope;
    auto m = router.match("DELETE", "/nope");
    during_404 = scope.count();
    is_404 = !m.matched() && !m.method_not_allowed();
  }
  CHECK(is_404);
  CHECK(during_404 == 0);
}

TEST_CASE("handle() performs no heap allocation, middleware included")
{
  // arena_task's coroutine frames come from a fixed arena (support.h), so
  // the counter sees only the router's own allocations. handle() builds
  // one continuation closure per request (the terminal) plus one per
  // middleware layer (the inner next); each is 4 pointers = 32 bytes and
  // must fit move_only_fn's inline buffer, or every request pays 1+N
  // heap allocations.
  using arena_router = http::router<httptest::arena_task, httptest::test_request, httptest::test_response>;
  arena_router router;
  router.get("/entity/:id", [](httptest::test_request&, httptest::test_response&, unsigned) -> httptest::arena_task {
    co_return;
  });
  for (int i = 0; i < 3; ++i)
    router.use(
      [](httptest::test_request&, httptest::test_response&, arena_router::next_fn next) -> httptest::arena_task {
        co_await next();
      }
    );

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7"); // warm up lazy initialisation

  long during = 0;
  {
    httptest::allocation_scope scope;
    router.handle(req, res, "GET", "/entity/7");
    during = scope.count();
  }
  CHECK(during == 0);
}

// --- move safety ---
//
// The router is movable: its trie holds unique_ptrs, so it is move-only
// but move-constructible and move-assignable. That makes moving a normal
// thing to do -- returning a configured router from a factory, holding
// one in a std::optional, moving one into a server object -- and it
// makes the moved-from router a normal thing to destroy.
//
// Everything the router stores past registration (bound handlers in the
// trie, the composed middleware chain) must stay valid when the router
// moves, so none of it may point at the router object itself. A bound
// handler refers to the heap-allocated error policy, whose address does
// not change. These tests run under AddressSanitizer, so breaking that
// invariant fails here as a heap-use-after-free rather than silently.

TEST_CASE("a moved router keeps its routes and its error policy usable")
{
  auto original = std::make_unique<httptest::router>();
  std::optional<http::param_error> reported;
  original->set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  original->get("/entity/:id", &entity_handler);

  httptest::router moved = std::move(*original);
  original.reset(); // destroy the source before dispatching

  httptest::test_request req;
  httptest::test_response res;

  // The route survived the move...
  auto match = moved.match("GET", "/entity/abc");
  REQUIRE(match.matched());

  // ...and so did the error policy it reports through.
  (*match.handler)(req, res, match.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
}

TEST_CASE("move assignment keeps routes and the error policy usable")
{
  auto source = std::make_unique<httptest::router>();
  std::optional<http::param_error> reported;
  source->set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  source->get("/entity/:id", &entity_handler);

  httptest::router target;
  target = std::move(*source);
  source.reset(); // destroy the source before dispatching

  httptest::test_request req;
  httptest::test_response res;
  auto match = target.match("GET", "/entity/xyz");
  REQUIRE(match.matched());
  (*match.handler)(req, res, match.captures);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);
}

TEST_CASE("handle() works on a moved router, middleware and not-found included")
{
  auto original = std::make_unique<httptest::router>();
  bool middleware_ran = false;
  original->use(
    [&middleware_ran](
      httptest::test_request&, httptest::test_response&, httptest::router::next_fn next
    ) -> httptest::test_task {
      middleware_ran = true;
      co_await next();
    }
  );
  std::optional<http::param_error> reported;
  original->set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  bool not_found_ran = false;
  original->set_not_found(
    [&not_found_ran](httptest::test_request&, httptest::test_response& res) -> httptest::test_task {
      not_found_ran = true;
      res.status(404);
      co_return;
    }
  );
  original->get("/entity/:id", &entity_handler);

  httptest::router moved = std::move(*original);
  original.reset();

  httptest::test_request req;
  httptest::test_response res;

  // A matched route with an unparsable segment: the bound handler runs
  // inside the onion and reports through the error policy.
  moved.handle(req, res, "GET", "/entity/abc");
  CHECK(middleware_ran);
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::bad_path);

  // The not-found path still works after the move too.
  res = httptest::test_response{};
  moved.handle(req, res, "GET", "/nope");
  CHECK(not_found_ran);
  CHECK(res.status() == 404);
}

TEST_CASE("task_like resolves operator co_await the way co_await does")
{
  // The object as its own awaiter (the fallback).
  static_assert(http::task_like<httptest::test_task>);
  // Member operator co_await.
  static_assert(http::task_like<member_co_task>);
  // Free operator co_await, found by ADL.
  static_assert(http::task_like<free_co_task>);

  // An awaiter bound to one specific promise type is still rejected:
  // its await_suspend does not accept a plain coroutine_handle<>.
  static_assert(!http::task_like<bound_task>);
  // await_suspend must return void, bool, or a coroutine handle.
  static_assert(!http::task_like<bad_suspend_task>);
  // Not a coroutine return type at all.
  static_assert(!http::task_like<int>);
}

TEST_CASE("a router over a member-co_await task type dispatches end to end")
{
  using co_router = http::router<member_co_task, httptest::test_request, httptest::test_response>;
  co_router router;

  bool ran = false;
  router.get(
    "/entity/:id", [&ran](httptest::test_request&, httptest::test_response& res, unsigned id) -> member_co_task {
      ran = true;
      CHECK(id == 7u);
      res.status(201);
      co_return;
    }
  );

  bool middleware_ran = false;
  router.use(
    [&middleware_ran](
      httptest::test_request&, httptest::test_response& res, co_router::next_fn next
    ) -> member_co_task {
      middleware_ran = true;
      co_await next();
      res.header("x-after", "done");
    }
  );

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/entity/7");

  CHECK(ran);
  CHECK(middleware_ran);
  CHECK(res.status() == 201);
  REQUIRE(res.header("x-after").has_value());
  CHECK(*res.header("x-after") == "done");
}

TEST_CASE("a router over a free-co_await task type dispatches end to end")
{
  using free_router = http::router<free_co_task, httptest::test_request, httptest::test_response>;
  free_router router;

  bool ran = false;
  router.get("/ping", [&ran](httptest::test_request&, httptest::test_response& res) -> free_co_task {
    ran = true;
    res.status(204);
    co_return;
  });

  httptest::test_request req;
  httptest::test_response res;
  router.handle(req, res, "GET", "/ping");

  CHECK(ran);
  CHECK(res.status() == 204);
}

TEST_CASE("a write-only response type (setters, no getters) works with the router end to end")
{
  using write_router = http::router<httptest::test_task, httptest::test_request, write_only_response>;
  write_router router;
  router.get("/entity/:id", [](httptest::test_request&, write_only_response& res, unsigned id) -> httptest::test_task {
    CHECK(id == 7u);
    res.status(207);
    co_return;
  });

  httptest::test_request req;
  write_only_response res;
  router.handle(req, res, "GET", "/entity/7");
  CHECK(res.status_code == 207);
}

TEST_CASE("an injected<T> parameter resolves from the context even when T has from_string")
{
  httptest::router router;
  std::optional<http::param_error> reported;
  router.set_param_error_handler(
    [&reported](httptest::test_request&, httptest::test_response&, http::param_error error) -> httptest::test_task {
      reported = error;
      co_return;
    }
  );
  router.get("/config", &config_handler);

  auto ctx = std::make_shared<http::context>();
  ctx->set<std::string>(std::make_shared<std::string>("prod"));
  httptest::test_request req;
  req.ctx = ctx.get();
  httptest::test_response res;
  seen_config.reset();
  router.handle(req, res, "GET", "/config");
  REQUIRE(seen_config.has_value());
  CHECK(*seen_config == "prod");
  CHECK_FALSE(reported.has_value());

  // Without the context entry it is a missing_injection, not a bad_path.
  httptest::test_request bare_req;
  httptest::test_response res2;
  seen_config.reset();
  reported.reset();
  router.handle(bare_req, res2, "GET", "/config");
  CHECK_FALSE(seen_config.has_value());
  REQUIRE(reported.has_value());
  CHECK(*reported == http::param_error::missing_injection);
}

TEST_CASE("a path parameter and an injected<T> parameter mix in one handler")
{
  httptest::router router;
  router.get("/entity/:id", &mixed_config_handler);

  auto ctx = std::make_shared<http::context>();
  ctx->set<std::string>(std::make_shared<std::string>("cfg"));
  httptest::test_request req;
  req.ctx = ctx.get();
  httptest::test_response res;
  seen_mixed_id.reset();
  seen_mixed_config.reset();
  router.handle(req, res, "GET", "/entity/7");
  REQUIRE(seen_mixed_id.has_value());
  CHECK(*seen_mixed_id == 7u);
  REQUIRE(seen_mixed_config.has_value());
  CHECK(*seen_mixed_config == "cfg");
}

TEST_CASE("resolve_params resolves an injected<T> element from the context")
{
  auto ctx = std::make_shared<http::context>();
  ctx->set<std::string>(std::make_shared<std::string>("prod"));

  http::capture_list none;
  auto resolved = http::resolve_params<std::tuple<http::injected<std::string>>>(none, ctx.get());
  REQUIRE(resolved.error == http::param_error::none);
  REQUIRE(resolved.values.has_value());
  CHECK(std::get<0>(*resolved.values).value == "prod");

  // A capture left over after resolving an injected<T> element is a
  // bad_path: the wrapper never consumes a segment.
  http::capture_list extra;
  extra.push({"id", "7"});
  auto leftover = http::resolve_params<std::tuple<http::injected<std::string>>>(extra, ctx.get());
  CHECK(leftover.error == http::param_error::bad_path);
  CHECK_FALSE(leftover.values.has_value());
}
