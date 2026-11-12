# httproute20

An asynchronous, typed HTTP request router for C++20. Route handlers
are coroutines: the router awaits them, and they can await anything
else — I/O, channels, other tasks. The library is coroutine-generic:
it constrains your task type with a concept instead of naming one, so
it works with any freely-awaitable C++20 coroutine type. Header-only,
zero dependencies besides the C++ standard library; requires RTTI and
exceptions.

Core features:

- **Typed handlers** — coroutine handlers whose trailing parameter
  count and classification (path vs injected) are checked at compile
  time; `:name` segments bind by position, not by name.
- **Compile-time pattern validation** — malformed patterns and
  `":name"`/parameter arity mismatches are consteval errors at the
  registration site.
- **Path parameters** — `":name"` segments parsed into handler
  parameters through `from_string<T>` (`unsigned`, `int`,
  `std::string`, `std::string_view` ship with the library).
- **Dependency injection** — non-path parameters resolve from a
  per-request `context` scope through `injector<T>`.
- **Trie matching** — per-method segment trie: a literal route beats a
  parameterised one regardless of registration order, backtracking
  handles shared prefixes, matching is O(path segments) with no
  per-match heap allocation; `handle()` adds no heap allocations of its
  own either (the per-request continuation closures fit the inline
  buffer of the router's move-only callable).
- **Full method coverage** — `route(method, ...)` plus
  `get`/`post`/`put`/`patch`/`del`/`head`/`options` aliases; each method
  has its own trie, so routes never collide across methods.
- **Catch-all segments** — a trailing `*name` segment captures the rest
  of the path (slashes included) as one parameter; literal and `:name`
  routes still win over it.
- **Middleware pipeline** — onion middleware via `use()`, composed once
  at startup and dispatched through `handle()`; short-circuiting and
  per-route decorators supported.
- **Your types** — generic over `Task`/`Req`/`Res` via concepts; one
  alias binds your concrete task, request, and response types.
- **No baked-in policy** — error responses and the not-found handler
  are injected, not owned.

The design walkthrough — the consteval pattern validation, the
handler-side concepts, the segment trie, the compile-time/runtime
split, known sharp edges, and the extension backlog — is in
[docs/design.md](docs/design.md).

## Quick start

The router is generic over your coroutine task type and your
request/response types. Bind them once with an alias:

```cpp
#include <http/router.h>

using router = http::router<my::task, my::request, my::response>;

router routes;
routes.get("/entity/:id", &get_entity);
// get_entity(my_request& req, my_response& res, unsigned id,
//            my::i_store& store)
```

`<http/router.h>` is the umbrella header; the layers it pulls in live
in their own self-contained headers under `include/http/` —
`captures.h`, `context.h`, `function.h`, `params.h` (parameter
resolution), `traits.h` (the shared concepts), `pattern.h` (pattern
validation) — so a consumer can include just what it needs. Each
header compiles standalone (CI syntax-checks every one with `-Werror`).

`get` is one of the verb aliases — `post`/`put`/`patch`/`del`/`head`/
`options` — and `route(method, ...)` registers any other method; each
method has its own trie. `Task` must satisfy `task_like`, `Req`
`request_like`, `Res` `response_like`. Handlers return `Task` and take
`(Req&, Res&, Params...)`; each trailing parameter is either:

- a **path parameter** — parsed from a `":name"` segment through a
  `from_string<T>` specialization (`unsigned`, `int`, `std::string`,
  `std::string_view` ship with the library); a custom specialization's
  `parse(std::string_view)` must return `std::optional<T>` (`nullopt`
  when the segment does not parse), or
- an **injected parameter** — resolved from the request's `context`
  scope through `injector<T>` (any type registered with
  `context::set<T>` is injectable as `T&` with no per-type code).

  Wrap a parameter in `http::injected<T>` to force injection even for a
  type with a `from_string` specialization — e.g. `injected<std::string>`
  for a config value, which a plain `std::string` parameter would parse
  from a `":name"` segment instead.

The pattern is validated at compile time: malformed `":name"` or
`"*name"` segments (a catch-all must be the last segment), empty
segments, trailing slashes, and parameter-count mismatches are
consteval errors at the registration site. Only the segment *count*
is checked against the handler — the `:name` text is decorative at the
type level, and the pattern never constrains the parameter type:
`/entity/:id` binds the segment to whatever type the handler declares
(`unsigned`, `std::string`, ...), parsed at request time. For a
diagnostic that names both counts, use the template-argument syntax
`routes.get<"/entity/:id">(&get_entity)`: there the pattern is a
constant expression and the arity check is a `static_assert` that
prints the two counts (`1 == 2`).

## The concepts

```cpp
// A coroutine return type that is freely awaitable: has a promise_type
// and co_await accepts it — directly, or via a member or free
// operator co_await — with the awaiter's await_suspend accepting a
// plain std::coroutine_handle<>. Any freely-awaitable type qualifies;
// awaiters bound to one specific promise type do not.
template<typename T>
concept task_like;

// The router reads one thing from a request: the per-request context
// scope injected parameters resolve from. A context is a handle to its
// scope (like shared_ptr): a const context* still allows set/get, so
// this is all middleware needs to register per-request entries via
// req.context()->set<T>().
template<typename Req>
concept request_like; // { req.context() } -> const context*

// The write contract handlers respond through. The router itself
// forwards Res opaquely.
template<typename Res>
concept response_like; // res.status(int), res.header(name, value)

// The read contract for post-processing middleware, which inspects the
// response after next() returns. The router never calls these —
// static_assert response_readable<Res> if your middleware needs them.
template<typename Res>
concept response_readable; // res.status() -> int,
                           // res.header(name) -> optional<string_view>
```

## Error policy

The router does not own an HTTP error policy. When a matched route's
parameters fail to resolve — a `":name"` segment that does not parse
(`param_error::bad_path`, a client error) or an injected parameter
missing from the context (`param_error::missing_injection`, a
configuration error) — it calls the consumer's callback:

```cpp
routes.set_param_error_handler(
  [](my_request& req, my_response& res, http::param_error e) -> my::task {
    res.status(e == http::param_error::bad_path ? 400 : 500);
    co_await res.send("...");
  });
```

Without a callback, a failed resolution produces no response.

## Matching semantics

Each HTTP method has its own segment trie, registered through
`route(method, ...)` or a verb alias. Within a trie, literal edges are
tried before the `":name"` edge, which is tried before a `"*name"`
catch-all edge — so a literal route always wins over a parameterised
one, and both win over a catch-all, regardless of registration order.
A catch-all matches one or more remaining segments and captures them
raw, slashes included. Backtracking covers patterns that share a
prefix but diverge later. Matching is O(path segments), not
O(routes x segments), and needs no heap — captures are a fixed-size
array. `match()` returns a `route_match` whose `handler` is a
non-owning pointer into the router's trie, valid while the router
lives; the match itself allocates nothing. When the path matches only
under other methods, `route_match::method_not_allowed()` is true and
`allowed()` lists those methods (sorted, non-owning views into the
router's method keys) — a 405, not a 404.

HEAD and OPTIONS derivation is the consumer's, not the router's.
Register `head("/entity/:id", &same_handler)` alongside `get(...)` —
the router dispatches HEAD to it like any other method, and the
transport suppresses the response body (RFC 9110 §9.3.2: a server must
not send content in a HEAD response; the router cannot do this itself
because `Res` is opaque beyond `status`/`header`). For OPTIONS, answer
inside `set_method_not_allowed`, which receives the allowed methods: an
`OPTIONS` request can be answered `204` + `Allow` (the request method
comes from your own `Req` type — `request_like` only requires
`context()`). Router-side derivation is possible — see the roadmap.

Patterns are capped at `max_segments` (16) segments and rejected at
compile time beyond that. A request path may be longer, but a pattern
without a catch-all matches only a path of exactly its own length;
extra segments make it fail rather than being ignored. A catch-all is
unaffected by path depth.

Query parameters are not part of the path and must be stripped off
before calling match.

Percent-encoding is the server's job too: `match()`/`handle()` take the
path exactly as given and never decode, so captures are the raw path
bytes (`/u/a%2Fb` captures `a%2Fb`, and a literal segment matches raw
text only). Decode in the server loop before calling the router —
captures then arrive decoded — and decide there what `%2F` means:
decoding before matching would turn it into a real `/` and change
segment boundaries, so many servers reject encoded slashes outright.
Keeping decoding out of the router is what keeps `match()`
allocation-free.

## Why no owned awaitable

Research into how coroutine libraries interoperate shows one clear
ecosystem pattern: interop happens at the language-level awaitable
protocol (`await_ready`/`await_suspend`/`await_resume` +
`std::coroutine_handle<>`), not through shared concrete task types —
cppcoro's awaitable traits, C++26 `std::execution::as_awaitable`, and
P4089R0's survey of nine task libraries (seven converged on plain
`task<T>`).

A library that owns its task type ends up owning its runtime
environment too (executor, cancellation) and needs bridges to every
other runtime. Asio's `awaitable<T, Executor>` is the canonical
cautionary tale — it is only conditionally awaitable (cppcoro #131),
and mixing task types needs per-pair bridges (51-105 lines each in the
cross_await survey).

httproute20 instead constrains on `task_like` and keeps the promise
the extension point: N task types + M awaitables compose as N+M
implementations instead of N x M bridges. `await_suspend` taking a
plain `std::coroutine_handle<>` — freely awaitable in any promise — is
the property that makes a task type work here with no adapter.

## Middleware: what Rack/WSGI/ASP.NET Core teach

Rack and WSGI reduce the web interface to one callable plus a mutable
per-request env dict, with middleware as the "onion": each middleware
wraps the next and either calls it or short-circuits, composed once at
startup. ASP.NET Core is the proof the pattern survives compilation:
`RequestDelegate` replaces the duck-typed callable, `HttpContext`
replaces the env dict, middleware takes `next` by constructor
injection, and the chain is reverse-folded into one nested delegate.

What does not survive compilation is the stringly-typed env dict — and
it does not need to: `http::context` is its typed replacement. Rack's
env is the cross-middleware communication channel (upstream middleware
stashes a user object under a string key, downstream reads it);
`context::set<T>`/`get<T>` is the same channel with the type as the
key, checked at compile time. FastAPI's dependency injection —
per-endpoint dependency graphs resolved as typed function arguments
with per-request caching — is the dynamic-language twin of this
library's `injector<T>`/`context` pair.

The pipeline is implemented as the onion pattern: a middleware is a
callable receiving `(Req&, Res&, next)` — or `(Req&, Res&, next,
const http::route_info&)`, where the fourth argument is what the
request matched (route template, raw captures, 404/405 flags) — and
`next` is a `Task()` invocable, registered with `use()` and composed
once at startup.
Handlers and middleware may be move-only — they are stored in
`http::move_only_fn`, a minimal move-only `std::function` replacement
(C++20 has no `std::move_only_function`, and `std::function` requires
a copyable target). `next` is move-only too: take it by value, it is
a single-shot continuation.

```cpp
routes.use([](my::request& req, my::response& res, router::next_fn next) -> my::task {
  co_await next();                        // run the rest of the onion
  log(req, res.status());                 // post-processing: Res has getters
});
routes.use([](my::request& req, my::response& res, router::next_fn next, const http::route_info& route) -> my::task {
  co_await next();
  metrics.observe(route.matched ? route.pattern : "404", res.status()); // route template, not raw path
});
routes.use([](my::request& req, my::response& res, auto next) -> my::task {
  if (!authorized(req)) {
    res.status(401);
    co_await res.send("unauthorized\n");
    co_return;                            // short-circuit: next not called
  }
  req.context()->set<session>(make_session(req)); // typed env channel
  co_await next();
});

co_await routes.handle(req, res, req.method(), req.path()); // whole dispatch
```

`handle()` matches, then runs the onion around the matched handler —
or around the method-not-allowed handler when the path matches only
under other methods (`set_method_not_allowed`, which receives the
allowed methods for the `Allow` header), or the not-found handler when
nothing matches, so middleware also observes 405s and 404s. A matched
route whose parameters fail to resolve still reports through the
param-error policy, from inside the onion.
Per-route middleware needs no router support: compose a decorator
around the handler before registration — `routes.get("/admin/:id",
require_session(&get_admin))` — the test suite shows this pattern
adding authentication to some routes only. Post-processing middleware
needs an inspectable response — `static_assert` that your `Res`
satisfies `response_readable` (the `status()`/`header(name)` getters);
`response_like` itself is only the write contract.

## Roadmap

All first-release must-haves are implemented: full method coverage,
catch-all segments, and the middleware pipeline.

Deferred: sub-router mounting (node grafting is mechanically cheap —
the walk needs no changes — but conflict semantics, catch-all
precedence, not-found scoping, and context ownership need answers
first) and query-parameter binding (queries stay parse-on-demand).

Also considered and deferred: router-side HEAD/OPTIONS derivation. A
request-time HEAD→GET fallback in `match()` (explicit `head()` routes
still win, still allocation-free) and automatic OPTIONS synthesis in
`handle()` (204 + `Allow` from the allowed-methods walk `match()`
already performs) are both mechanically possible — but HEAD body
suppression can never happen inside the router (`response_like` is
only `status`/`header`; the body write is the consumer's own `Res`
API), so derivation would be opt-in at best. The consumer-side recipe
in "Matching semantics" covers it today.