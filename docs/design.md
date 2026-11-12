# Design

How the typed router works: the compile-time pattern validation, the
handler-side concepts, the segment trie, and the parameter-resolution
classification. Usage is in the [README](../README.md); this document
is the design walkthrough.

## Motivation

The classic alternative to a typed router matches URL patterns like
`/entity/:id` against request paths and hands the handler an
`unordered_map<string, string>` of parameters. That is flexible but
unchecked: nothing ties a pattern to a handler signature, a missing or
misspelled parameter surfaces as an empty string at runtime, and every
parameter is a string until the handler converts it.

This library explores the opposite corner: the pattern is a
compile-time entity, and the compiler checks that the pattern and the
handler agree.

```cpp
my::task get_entity(my::request&, my::response& res, unsigned id)
{
  res.status(200);
  res.header("content-type", "application/json");
  co_await res.send("{\"id\":" + std::to_string(id) + "}\n");
}

routes.get("/entity/:id", &get_entity);
```

This only compiles if the number of `:name` segments in the pattern
matches the number of the handler's path parameters. At request time
each segment is converted to the handler's parameter type; a segment
that matches the pattern shape but fails to convert (e.g.
`/entity/abc` for an `unsigned id`) reaches the consumer's error
policy (`param_error::bad_path`) instead of silently passing an empty
value.

## How it works

The library is header-only and builds on C++20 `consteval` and
concepts. `<http/router.h>` is the umbrella; the layers live in their
own self-contained headers under `include/http/`, in dependency order:
`captures.h` (capture storage), `context.h` (the DI scope),
`function.h` (move_only_fn), `params.h` (parameter resolution),
`traits.h` (the shared concepts and handler-signature traits),
`pattern.h` (pattern validation), and `router.h` itself (dispatch:
the per-method trie, the middleware onion). Each header compiles
standalone — CI syntax-checks every one with warnings as errors.

1. **`typed_pattern<Handler>`** — a route pattern validated at compile
   time. Its `consteval` constructor splits the literal, rejects
   malformed `:name` segments, accepts a single trailing `*name`
   catch-all segment (which must be last), and checks that the
   `:name`/`*name` count matches the handler's trailing parameter count.
   The results are stored in the object: computed at compile time, but
   runtime *data*.

   This is the one place where the literal syntax trades precision for
   ergonomics. A function parameter can never be a constant expression,
   so with `routes.get("/entity/:id", &handler)` the pattern is runtime
   data and the arity check surfaces as a non-constant `consteval` call
   rather than a concept failure. The template-argument syntax
   `routes.get<"/entity/:id">(&handler)` is also provided
   (`pattern_text`, a C++20 structural NTTP): it keeps the pattern in
   the type system, and the arity check becomes a `static_assert` that
   names both counts — at the cost of the template syntax at the call
   site.

2. **`handler_traits<Handler>`** — extracts the argument tuple from a
   handler signature. Free functions, function pointers and
   lambdas/functors (through `operator()`) are supported. The primary
   template is defined but empty, so unsupported types fail the
   concepts below by substitution failure instead of a hard error.

3. **The handler-side concepts** — `route_handler`,
   `request_response_handler` and `non_member_pointer` constrain
   `router::route` and its verb aliases (`get`/`post`/`put`/...);
   `route_handler` checks the return type against `task_like`. They depend only on the handler, never on the pattern,
   so they keep precise diagnostics: a violating registration is
   rejected at the caller's line with each unsatisfied constraint
   named.

4. **`router`** — a segment trie, one per method. Literal edges are
   tried before the `:name` edge, which is tried before a `*name`
   catch-all edge, so a literal route always wins over a parameterised
   one and both win over a catch-all, regardless of registration order,
   and backtracking covers patterns that share a prefix but diverge later.
   A catch-all consumes the rest of the path (slashes included) as one
   capture and matches at least one segment. Matching is O(path
   segments) rather than O(routes x segments), and shared prefixes share
   nodes.

5. **`bind_route`** — the glue, a `router` member. It wraps the typed
   handler into a `handler_fn` that receives the captures the trie
   collected, converts each through `from_string<T>`, and calls the
   handler with typed values. A resolution failure is reported through
   the router's error callback: `bad_path` for a segment that fails to
   parse, `missing_injection` for an injected parameter absent from the
   context.

6. **`from_string<T>` / `injector<T>`** — the parameter classification.
   `from_string` is a user-specializable conversion trait: integrals
   parse strictly with `std::from_chars` (the whole segment must be
   consumed); `std::string` and `std::string_view` are built in. New
   parameter types are added by specializing the trait — no router
   changes needed.

The bound wrapper produces a `handler_fn`
(`move_only_fn<Task(Req&, Res&, const capture_list&)>` — a minimal
move-only `std::function` replacement in `http/function.h`; C++20 has
no `std::move_only_function`, and `std::function` requires a copyable
target, which would reject move-only handlers), so the consumer's
dispatch loop and the not-found path are unchanged: they see a plain
callable. `route_match::handler` is a non-owning pointer to the trie
node's `handler_fn`, valid while the router lives — that is what keeps
`match()` allocation-free. The ordered capture list goes to the handler for
typed conversion — the consteval arity check guarantees every `:name`
segment is delivered as a typed parameter, by position (the name text is
not checked). Middleware that needs the route template or the raw
captures gets them as the fourth argument, `const route_info&` —
non-owning views into the trie and the match, valid for the duration
of the dispatch — so access logging sees "/entity/:id", not the raw
path, without re-matching. The path is split once, by the matcher —
the wrapper does not re-split or re-match it.

### The parameter classification

A trailing handler parameter is a *path* parameter iff
`from_string<std::remove_cvref_t<T>>::parse` is well-formed, takes the
segment by value, and returns `std::optional<T>` (the primary template
is undefined, so this is a concept check); every other trailing
parameter is *injected* and resolved through
`injector<T>::resolve(const context*)`. Wrapping a parameter in
`injected<T>` opts it out of parsing explicitly — it is always injected,
even when `from_string<T>` exists (e.g. `injected<std::string>` for a
config value), so the two roles never compete silently. The default `injector` is a
plain context lookup, so `ctx.set<i_service>(impl)` makes `i_service&`
injectable with no per-type code; specialize `injector<T>` for derived
resolutions (e.g. a per-request transaction checked out of an
application-level pool). An injected parameter that resolves to
nothing is a misconfiguration and reaches the error policy as
`missing_injection`; a bad path segment outranks it, so the client
error is reported rather than the configuration error.

`context` is a type-erased scope (`type_index` → `shared_ptr<void>`)
with `set<T>(shared_ptr<T>)` / `get<T>() -> T*` and a parent pointer;
`get` walks child → parent, so a per-request child scope can shadow
application-level entries. A context is a handle to its scope, like
`shared_ptr`: constness of the context object does not propagate to the
entries — `get` returns a mutable `T*` from a const context and `set`
is const, so the `const context*` that `request_like` guarantees is
enough for middleware to register per-request entries through
`req.context()->set<T>(...)`.

### Compile time vs runtime

| Concern                                   | When        |
|-------------------------------------------|-------------|
| Pattern syntax (empty `:name`, empty or misplaced `*name`, empty segments, trailing `/`, too many segments) | compile time (`consteval`) |
| `:name`/`*name` count == handler path parameter count | compile time (`consteval`) |
| Handler takes `(Req&, Res&, ...)` | compile time (concept) |
| Middleware is `Task(Req&, Res&, next)` | compile time (concept `middleware_like`) |
| Handler returns a `task_like` type | compile time (concept) |
| Parameter type is convertible | compile time (trait must exist) |
| Parameter is path vs injected | compile time (from_string trait) |
| Injected service present in the context | runtime → error policy |
| Segment splitting | compile time (`consteval`), stored as data |
| Literal segments match the request path | runtime |
| Segment value parses (e.g. `unsigned`) | runtime → error policy |
| No route matches | runtime → not-found handler |

The split of responsibilities is deliberate: *structure* is checked at
build time, *values* can only be checked when a request arrives. Note
the seam: every check that depends only on the handler is a concept
and keeps a precise diagnostic, while the checks that depend on the
pattern degrade to a `consteval` rejection.

### The per-request lifecycle

```
startup:     app_ctx->set<i_entity_store>(store);
              routes.set_context(app_ctx);
per request:  attach a child of the application context to the request
              req.context()->set<request_id>(generate());  // request scope only
dispatch:     id    <- from_string<unsigned>::parse("42")  // path parameter
              store <- injector<i_entity_store>::resolve(req.context())
                    = req.context()->get<i_entity_store>()  // child miss -> parent hit
end:          the request-scoped child is destroyed with the request
```

Per-request state (request ids, tracing spans, transactions) lives in
the request-scoped child: it is owned by a single task (no locking)
and never leaks across requests. Application-level entries are shared
and must be immutable or externally synchronized.

### Known sharp edges

- The library requires RTTI and exceptions: `context` keys its entries
  by `std::type_index(typeid(T))`, and `typed_pattern`'s consteval
  validator reports malformed patterns with `throw`. Builds with
  `-fno-rtti` or `-fno-exceptions` therefore fail.
- Generic (`auto`) lambdas are not supported as handlers — the argument
  types must be fixed for the arity check; they fail the
  `route_handler` concept at registration.
- A `std::string_view` parameter borrows from the request path and is
  only valid for the handler's lifetime.
- Custom `from_string`/`injector` specializations carry an unchecked
  semantic contract: `parse`/`resolve` must be equality-preserving and
  side-effect-free. The signature is checked, not just semantic: `parse`
  must take its segment by value and return `std::optional<T>` (the
  `path_parameter` concept pins the by-value parameter with explicit
  const and rvalue calls, and the return type with `same_as`).
  Violations of the semantics satisfy the concept without modeling it —
  ill-formed, no diagnostic required.
- With the literal syntax, a pattern/handler arity mismatch is reported
  as a non-constant `consteval` call: the compiler names the pattern
  and shows the `throw` line carrying the message, but not the two
  counts that disagree — a function parameter is never a constant
  expression, so the pattern count cannot reach a template argument.
  The template-argument syntax `routes.get<"/entity/:id">(&handler)`
  turns the check into a `static_assert` that names both counts
  (Clang: "expression evaluates to '1 == 2'"; GCC: "the comparison
  reduces to '(1 == 2)'"; MSVC prints the message and the expanded
  `pattern_text<12>{...}` argument).
- Two patterns with the same shape but different parameter names
  (`/entity/:id` and `/entity/:name`) collide on the same trie edge:
  the later registration replaces the earlier handler. Nothing detects
  this yet.
- The `:name` text is decorative at the type level: the consteval check
  compares only the segment *count*, names bind by position, and the
  pattern never constrains the parameter type — `/entity/:id` binds
  the segment to whatever type the handler declares, parsed at request
  time.
- A request that matches a pattern's shape but fails to *parse* a
  segment reaches the error policy (`bad_path`) — the router does not
  fall through to a later route that might have matched. Literal routes
  are tried first, so `/entity/latest` is safe from `/entity/:id`, but
  two parameterised patterns of the same shape still shadow each
  other.
- `bool` is excluded from the integral `from_string` specialization
  (`std::from_chars` has no `bool` overload): a `bool` handler
  parameter is not a path parameter, so a pattern with a `:name`
  segment for it fails the consteval arity check at registration.
  `char` still falls under the integral specialization and parses
  integer notation — `"65"` yields `'A'`, `"a"` fails to parse —
  which is almost never what a path segment means. Prefer
  `unsigned`/`int` for numeric segments and `std::string` for raw
  ones; parse booleans in the handler.
- Patterns are capped at `max_segments` (16) segments; a longer
  pattern is rejected at compile time. A request path may be longer,
  but a pattern without a catch-all matches only a path with exactly
  as many segments as the pattern has — extra segments make it fail
  rather than being ignored. A catch-all is unaffected by path depth.
- The query string is not part of the path: `match` strips everything
  from the first `?` before splitting, so a capture never contains it —
  `/entity/7?expand=all` captures `7`. Query parameters are not bound
  to handler parameters; that is deferred (parse-on-demand).
- The router never percent-decodes: `match()`/`handle()` take the path
  exactly as given, so captures are raw bytes (`/u/a%2Fb` captures
  `a%2Fb`) and a literal segment matches raw text only. Decoding is the
  server's job, done before the router is called — which is also where
  the `%2F` policy lives, since decoding before matching would turn an
  encoded slash into a real `/` and change segment boundaries (many
  servers reject `%2F` outright). Keeping decoding out of the router is
  what keeps `match()` allocation-free.
- A `*name` catch-all captures the raw remainder of the path — slashes
  included — as one parameter, and matches one or more segments:
  `/a/*x` does not match `/a`. It is tried after the literal and
  `:name` edges, so more specific routes win; a numeric handler
  parameter for it fails to parse at request time (`bad_path`).

## Middleware

Middleware is implemented as the onion pipeline: `router::use`
registers a callable `(Req&, Res&, next)` or `(Req&, Res&, next,
const route_info&)` (`next` a move-only `Task()` invocable,
`http::next_fn`; the 3-argument form is wrapped at registration), the
chain is reverse-folded once per
`use()` call (startup), and `router::handle(req, res, method, path)`
matches and
runs the chain around the matched handler — or around the
method-not-allowed handler when the path matches only under other
methods, or the not-found handler when nothing matches, so middleware
also observes 405s and 404s. A matched route whose parameters fail to
resolve still reports through the param-error policy, from inside the
onion. Each method has its own trie, so on a miss `match()` walks the
other tries and reports the allowed methods on the `route_match`
(`method_not_allowed()`, `allowed()` — sorted, non-owning views into
the method keys, still allocation-free); `handle()` passes that list to
the 405 handler for the `Allow` header. `handle()` also builds the
`route_info` (pattern, captures, matched/405 flags — all non-owning
views) and threads it through the chain to every middleware, so the
per-request continuation closures stay within `move_only_fn`'s inline
buffer and `handle()` remains allocation-free.

HEAD and OPTIONS derivation is deliberately the consumer's, not the
router's: `head()` can delegate to the GET handler, and the 405 handler
receives the allowed methods, so it can answer OPTIONS with 204 + Allow
(the request method comes from the consumer's own `Req` type).
A request-time HEAD→GET fallback in `match()` and OPTIONS synthesis in
`handle()` are mechanically possible, but the router can never suppress
the HEAD body — `response_like` exposes only `status`/`header`, and the
body write is the consumer's own `Res` API — so per RFC 9110 §9.3.2 the
transport owns body suppression and derivation stays consumer-side.

```cpp
routes.use([](Req& req, Res& res, router::next_fn next) -> Task {
  co_await next();
  log(req, res.status()); // post-processing needs the Res getters
});
```

Two consequences of the compiled mapping:

- Cross-middleware state is the per-request `context`: upstream
  middleware does `req.context()->set<T>(...)`, downstream middleware
  reads it, and a handler parameter can inject it through
  `injector<T>` — Rack's env dict with the type as the key.
- Post-processing middleware needs an inspectable response: the
  `status()`/`header(name)` getters are the separate `response_readable`
  contract, which the router never calls — `response_like` is only the
  write contract (`status(int)`, `header(name, value)`).

A middleware short-circuits by responding without calling `next` —
that is the whole mechanism, no special API.

Per-route middleware is a decorator composed around the typed handler
before binding — no router changes needed, and the consteval arity
check still applies to the innermost handler:

```cpp
routes.get("/admin/:id", require_session(&get_admin)); // some routes only
```

When both are used, the `use()` pipeline is the outer onion and the
decorated handler is the terminal. Route groups
(`routes.group("/api/v1", auth_middleware)`) remain a thin loop over
registrations with a composed handler; prefix-scoped middleware lists
are deferred with sub-router mounting.

## Testing the compile-time layer

Compile-time guarantees need two kinds of tests:

- **Positive checks** — plain `static_assert` inside doctest test cases.
  doctest has no `STATIC_REQUIRE` macro (it is a Catch2 feature; the
  doctest equivalent is an open feature request, issue #250), and a
  failing `static_assert` surfaces as a compile error of the whole test
  binary rather than a reported test case — acceptable, since such a
  failure is loud and points at the exact line.
- **Negative checks** (code that must *fail* to compile) — CTest's
  `WILL_FAIL` alone cannot verify the failure *reason*, and combining
  `WILL_FAIL` with `PASS_REGULAR_EXPRESSION` inverts the regex check as
  well. Instead, each must-fail source lives in
  `tests/compile_fail/` as an `EXCLUDE_FROM_ALL` target, and the test
  runs `cmake/compile_fail_test.cmake`, which passes only when the
  build fails **and** the compiler output contains the expected
  diagnostic. The anchors are chosen for portability: the name of the
  deepest unsatisfied concept for the handler-side checks (GCC and
  Clang also print the constraint expression, MSVC prints only the
  concept name), the `routable_params` concept name for an injected
  parameter whose `injector<T>` cannot resolve it, and
  `is not a constant expression` for the literal-syntax pattern checks,
  which are `consteval` rejections rather than concept failures. The
  template-argument syntax (`arity_mismatch_nttp`) instead anchors on
  the `static_assert` message text (`does not match the handler`), which
  GCC, Clang and MSVC all print. Each source
  also defines a `main()`, so that if a regression ever makes the file
  compile, the link succeeds and the test fails properly instead of
  passing via an unrelated link error.

## Extension backlog for the metaprogramming layer

- **Duplicate pattern detection** — a constexpr scan that turns two
  identical (or shape-ambiguous) patterns into a compile error instead
  of a silent first-match-wins.
- **Reverse routing** — generating URLs (`url_for("/entity/:id", 42)`)
  so link construction stays in sync with route definitions. The pattern
  is runtime data now, so this would be a runtime formatter rather
  than a compile-time `url_for<Pattern>`; keeping the pattern in the
  type system (the `routes.get<"/entity/:id">` form) is what would make
  the compile-time version possible.
- **Richer pattern syntax** — optional segments or constrained
  parameters (`"/entity/:id<[0-9]+>"`), all resolvable at compile time
  into the existing segment model. (`*name` catch-all tails are
  implemented.)
- **OpenAPI-style description generation** — patterns and handler
  parameter types are already first-class compile-time data; emitting
  a route table (method, pattern, parameter names and types) for
  documentation or client generation is mostly serialization.
- **Compile-time route tables** — registering all routes in one
  `constexpr` list would enable whole-table checks (uniqueness,
  coverage) and remove the per-route runtime registration cost.