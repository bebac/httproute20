#ifndef HTTPROUTE20_ROUTER_H
#define HTTPROUTE20_ROUTER_H

#include "http/captures.h"
#include "http/context.h"
#include "http/function.h"
#include "http/params.h"
#include "http/pattern.h"
#include "http/segment_decoder.h"
#include "http/traits.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace http
{
  // Typed route registration, path dispatch, and dependency injection
  // for HTTP servers — generic over the consumer's coroutine task and
  // request/response types, with no dependencies of its own.
  //
  //   using router = http::router<my::task, my::request, my::response>;
  //   router routes;
  //   routes.get("/entity/:id", &get_entity);
  //   // get_entity(req, res, unsigned id, my::i_store& store)
  //
  // This header is the umbrella: it includes the layers below and adds
  // the dispatch layer itself. In dependency order:
  //
  //   - captures.h: capture/capture_list, the fixed-size capture storage;
  //   - context.h: the type-erased service scope injected parameters
  //     resolve from;
  //   - function.h: move_only_fn, the single-shot continuation type;
  //   - params.h: parameter resolution — from_string/path_parameter and
  //     injector/injectable classification, resolve_params;
  //   - traits.h: the shared concepts (task_like, request_like,
  //     response_like, middleware_like, ...) and handler-signature traits;
  //   - pattern.h: pattern validation — typed_pattern's consteval
  //     constructor and the pattern_text template-argument syntax;
  //   - segment_decoder.h: the segment_decoder concept and the identity
  //     default — how a router decodes percent-escaped paths;
  //   - here: dispatch — router, a per-method segment trie, plus the
  //     middleware onion (use/handle) composed around the matched handler
  //     at dispatch time.
  //
  // They are one component: the trie stores typed_pattern segments and
  // bind_route output, and handler_fn/capture_list are the router's own
  // interface types.
  //
  // Namespace note: http::context is a dependency-injection scope
  // (context.h), not a protocol-level connection context.

  namespace detail
  {
    // Transparent hash/equal for string keys: the per-method tries own
    // their method keys as std::string (registration may pass any
    // string_view), while match() looks the method up by std::string_view
    // with no allocation (C++20 heterogeneous lookup).
    struct string_hash
    {
      using is_transparent = void;
      size_t operator()( std::string_view value) const noexcept
      {
        return std::hash<std::string_view>{}(value);
      }
    };
    struct string_equal
    {
      using is_transparent = void;
      bool operator()( std::string_view lhs, std::string_view rhs) const noexcept
      {
        return lhs == rhs;
      }
    };
  } // namespace detail

  // Path-based dispatch over a per-method segment trie. Literal edges are
  // tried before the ":name" edge, which is tried before a "*name"
  // catch-all edge, so a literal route always wins over a parameterised
  // one and both win over a catch-all, regardless of registration order;
  // backtracking covers patterns that share a prefix but diverge later.
  //
  // Matching is O(path segments) rather than O(routes x segments), and
  // shared prefixes share nodes.
  //
  // Middleware: use() registers onion middleware — callables
  // (Req&, Res&, next) composed once at startup — and handle() runs the
  // chain around the matched handler (or the not-found handler).
  //
  // Task/Req/Res are the consumer's coroutine task, request, and
  // response types, constrained by task_like/request_like/response_like.
  //
  // Decoder rewrites percent-escaped path segments (percent-decoding in
  // the typical case) and is the identity by default: raw matching,
  // exactly the pre-decoder behavior. With a real decoder, a literal
  // segment that misses on the raw text is retried with its decoded
  // form — so a pattern written in plain text matches both wire forms
  // of an escaped segment — and the matched captures are decoded
  // before middleware and handlers see them. Patterns themselves are
  // always written in plain, decoded text.
  template<typename Task, typename Req, typename Res, segment_decoder Decoder = identity_segment_decoder>
    requires task_like<Task> && request_like<Req> && response_like<Res>
  class router
  {
  public:
    // A bound route handler. The router passes the captures it collected
    // while matching; the wrapper converts them to the handler's trailing
    // parameter types. Move-only (move_only_fn), so handlers may capture
    // move-only state; a closure that fits the inline buffer is stored
    // without allocating.
    using handler_fn = move_only_fn<Task(Req&, Res&, const capture_list&)>;

    // The not-found handler never sees path parameters, so it keeps the
    // plain (Req&, Res&) shape instead of handler_fn.
    using not_found_fn = move_only_fn<Task(Req&, Res&)>;

    // The 405 handler: same shape, plus the allowed methods to put in
    // the Allow header.
    using method_not_allowed_fn = move_only_fn<Task(Req&, Res&, std::span<const std::string_view>)>;

    // The consumer's parameter-resolution error policy: called when a
    // matched route's parameters fail to resolve. bad_path is a client
    // error, missing_injection a configuration error — the library
    // leaves the response to the callback. Without one, a failed
    // resolution produces no response.
    using param_error_fn = move_only_fn<Task(Req&, Res&, param_error)>;

    // The middleware pipeline (Rack/WSGI/ASP.NET Core "onion"): a
    // middleware is a callable (Req&, Res&, next) or (Req&, Res&, next,
    // route_info) where next is a Task() invocable for the rest of the
    // chain. Each middleware either calls next — wrapping the rest of
    // the chain around itself — or short-circuits by responding without
    // calling it. The chain is composed once when use() registers
    // (startup), not per request.
    using next_fn = http::next_fn<Task>;
    using middleware_fn = move_only_fn<Task(Req&, Res&, next_fn, const route_info&)>;

    // Upper bound on how many allowed methods a 405 match lists. Fixed so
    // that match() stays allocation-free even on a miss.
    static constexpr size_t max_allowed_methods = 16;

    struct route_match
    {
      // Non-owning pointer into the router's trie: the match is valid
      // while the router lives (trie nodes are heap-allocated and never
      // moved). match() copies nothing but this pointer, so it stays
      // allocation-free.
      const handler_fn* handler = nullptr;
      capture_list captures = {};
      std::string_view pattern = {};
      // Set when the path matches under other methods but not the
      // request's — a 405, not a 404. The methods that do match, sorted,
      // as non-owning views into the router's method keys (valid while
      // the router lives). At most max_allowed_methods are listed;
      // method_not_allowed() is true regardless of truncation.
      std::array<std::string_view, max_allowed_methods> allowed_methods = {};
      size_t allowed_count = 0;

      bool matched() const
      {
        return handler != nullptr;
      }

      bool method_not_allowed() const
      {
        return handler == nullptr && allowed_count != 0;
      }

      std::span<const std::string_view> allowed() const
      {
        return {allowed_methods.data(), allowed_count};
      }

      // The middleware-facing view of this match: everything but the
      // handler pointer.
      route_info info() const
      {
        return {pattern, {captures.begin(), captures.end()}, matched(), method_not_allowed(), allowed()};
      }
    };

    // Typed routes: the pattern is a string literal, validated at compile
    // time (see typed_pattern). Each ":name" segment is converted to one of
    // the handler's path parameters; the handler's other trailing
    // parameters are injected from the request context.
    //
    // The pattern parameter is deliberately non-deduced (type_identity_t)
    // so the string literal converts into it instead of being matched
    // against it — that is what allows the literal syntax at the call
    // site.
    //
    // route() is the general form: the method string is copied into the
    // per-method trie's key, so any string_view is safe to register with,
    // and match() looks the method up by string_view with no allocation.
    // The verb aliases below are one-line delegates to it.
    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void route(std::string_view method, const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      insert(method, pattern, bind_route(std::move(handler)));
    }

    // The template-argument pattern syntax: the pattern is a constant
    // expression here, so the arity check is a static_assert naming both
    // counts instead of the consteval throw the literal syntax produces.
    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void route(std::string_view method, Handler handler)
    {
      static_assert(P.path_param_count() == detail::path_param_count_v<Handler>,
        "route pattern ':name'/'*name' count does not match the handler's path parameters");
      insert(method, typed_pattern<Handler>{P.value, unchecked_pattern_t{}}, bind_route(std::move(handler)));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void get(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("GET", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void get(Handler handler)
    {
      route<P>("GET", std::move(handler));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void post(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("POST", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void post(Handler handler)
    {
      route<P>("POST", std::move(handler));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void put(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("PUT", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void put(Handler handler)
    {
      route<P>("PUT", std::move(handler));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void patch(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("PATCH", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void patch(Handler handler)
    {
      route<P>("PATCH", std::move(handler));
    }

    // delete is a keyword, so the DELETE alias is spelled del.
    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void del(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("DELETE", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void del(Handler handler)
    {
      route<P>("DELETE", std::move(handler));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void head(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("HEAD", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void head(Handler handler)
    {
      route<P>("HEAD", std::move(handler));
    }

    template<typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void options(const std::type_identity_t<typed_pattern<Handler>>& pattern, Handler handler)
    {
      route("OPTIONS", pattern, std::move(handler));
    }

    template<pattern_text P, typename Handler>
      requires detail::request_response_handler<Handler, Req, Res> && detail::routable_params<Handler>
    void options(Handler handler)
    {
      route<P>("OPTIONS", std::move(handler));
    }

    // Registers a middleware at the end of the pipeline and recomposes
    // the onion. Like route registration, this is not synchronized for
    // concurrent modification: call it before serving.
    template<typename M>
      requires middleware_like<M, Task, Req, Res>
    void use(M middleware)
    {
      if constexpr (std::invocable<M&, Req&, Res&, next_fn, const route_info&>)
        middlewares_.push_back(std::make_shared<middleware_fn>(std::move(middleware)));
      else
        // The 3-argument form: wrapped so the chain's uniform
        // 4-argument signature simply drops the route_info.
        middlewares_.push_back(std::make_shared<middleware_fn>(
          [mw = std::move(middleware)](Req& req, Res& res, next_fn next, const route_info&) -> Task {
            co_await mw(req, res, std::move(next));
          }));
      rebuild_pipeline();
    }

    void set_not_found(not_found_fn handler)
    {
      not_found_ = std::move(handler);
    }

    const not_found_fn& not_found() const
    {
      return not_found_;
    }

    void set_method_not_allowed(method_not_allowed_fn handler)
    {
      method_not_allowed_ = std::move(handler);
    }

    const method_not_allowed_fn& method_not_allowed() const
    {
      return method_not_allowed_;
    }

    // Bound handlers point at the policy object rather than at the
    // router, so a callback set here also applies to routes registered
    // before it. Set it before serving: like route registration, it is
    // not synchronized for concurrent modification.
    void set_param_error_handler(param_error_fn handler)
    {
      *ensure_param_error() = std::move(handler);
    }

    const param_error_fn& param_error_handler() const
    {
      static const param_error_fn none;
      return param_error_ ? *param_error_ : none;
    }

    // The application context: services registered once, e.g. in main,
    // shared by every request. Must be immutable or externally
    // synchronized — it is read from every request task.
    void set_context(std::shared_ptr<http::context> ctx)
    {
      app_context_ = std::move(ctx);
    }

    const std::shared_ptr<http::context>& context() const
    {
      return app_context_;
    }

    // A fresh per-request scope: a child of the application context.
    // The server attaches one to every request; per-request state
    // (request id, span, transaction) is set on it and never leaks
    // across requests.
    std::shared_ptr<http::context> new_request_context() const
    {
      return std::make_shared<http::context>(app_context_);
    }

    route_match match(std::string_view method, std::string_view path) const
    {
      const auto segments = split_path(path);
      capture_list captures;
      // Scratch for the decoded-segment retry on a literal miss (see
      // find_literal). Only a non-identity decoder with a '%' in the
      // segment ever touches it; match() stays allocation-free either
      // way.
      std::array<char, decode_scratch_size> decode_scratch;

      if (const auto root = roots_.find(method); root != roots_.end())
      {
        if (const node* hit = walk(&root->second, segments, 0, path, captures, decode_scratch))
        {
          route_match result;
          result.handler = &hit->handler;
          result.captures = captures;
          result.pattern = hit->pattern;
          return result;
        }
      }

      // No route under this method: walk the other methods' tries so a
      // wrong-method request is distinguishable from an unknown path (a
      // 405 with an Allow header, not a 404). The scan reuses the split
      // segments and allocates nothing; the recorded methods are views
      // into the method keys, which are stable while the router lives.
      route_match result;
      capture_list scratch;
      for (const auto& [other_method, trie] : roots_)
      {
        if (other_method == method)
          continue;
        scratch.count = 0;
        if (walk(&trie, segments, 0, path, scratch, decode_scratch) != nullptr && result.allowed_count < max_allowed_methods)
          result.allowed_methods[result.allowed_count++] = other_method;
      }
      std::sort(result.allowed_methods.begin(), result.allowed_methods.begin() + static_cast<std::ptrdiff_t>(result.allowed_count));
      return result;
    }

    // Dispatch: match, then run the middleware onion around the matched
    // handler — or around the method-not-allowed handler when the path
    // matches only under other methods, or the not-found handler when
    // nothing matches, so middleware also observes 405s and 404s. The
    // consumer's server loop is this one call. A matched route whose
    // parameters fail to resolve still reports through the param-error
    // policy, from inside the onion.
    Task handle(Req& req, Res& res, std::string_view method, std::string_view path) const
    {
      auto m = match(method, path);

      // Percent-decoding (non-identity decoders only): the matched
      // captures decode into this frame's arena before the onion, so
      // middleware (route_info) and the handler see the same decoded
      // values. A capture that cannot decode — malformed input, or an
      // arena too small — is a bad_path param error: it reports through
      // the param-error policy from inside the onion, like every other
      // parameter-resolution failure. The identity decoder skips all of
      // it; the raw captures go straight through.
      [[maybe_unused]] std::array<char, capture_arena_size> arena;
      [[maybe_unused]] capture_list decoded;
      const capture_list* effective = &m.captures;
      if constexpr (!std::is_same_v<Decoder, identity_segment_decoder>)
      {
        if (m.matched())
          effective = decode_captures(m.captures, decoded, arena) ? &decoded : nullptr;
      }
      const capture_list& info_captures = effective ? *effective : m.captures;
      const route_info info{
        m.pattern, {info_captures.begin(), info_captures.end()}, m.matched(), m.method_not_allowed(), m.allowed()};

      // The terminal keeps move_only_fn's inline buffer: this, the two
      // references, and one pointer to the frame-local dispatch state
      // are exactly four words (see bind_route).
      struct dispatch_state
      {
        const route_match* match;
        const capture_list* captures; // null: a capture failed to decode
      } state{&m, effective};
      next_fn terminal = [this, &req, &res, &state]() -> Task {
        if (state.captures == nullptr)
        {
          if (param_error_ && *param_error_)
            co_await (*param_error_)(req, res, param_error::bad_path);
        }
        else if (state.match->matched())
          co_await (*state.match->handler)(req, res, *state.captures);
        else if (state.match->method_not_allowed() && method_not_allowed_)
          co_await method_not_allowed_(req, res, state.match->allowed());
        else if (not_found_)
          co_await not_found_(req, res);
      };
      if (pipeline_)
        co_await (*pipeline_)(req, res, terminal, info);
      else
        co_await terminal();
    }

    // Binds a typed handler to a handler_fn. The handler must be callable
    // as
    //
    //   Task handler(Req&, Res&, Params...)
    //
    // where each Param is either a path parameter (their number equals
    // the number of ":name"/"*name" segments in the pattern, and each has
    // a from_string specialization) or an injected parameter resolved from
    // the request context through injector<T>. The pattern arity check
    // happens in typed_pattern's consteval constructor; the handler-side
    // checks are concepts on router::get. At request time a resolution
    // failure is reported through the router's param_error callback —
    // the response policy is the consumer's.
    template<typename Handler>
    handler_fn bind_route(Handler handler)
    {
      using params = detail::handler_params<Handler>;
      // The closure captures the handler and a raw pointer to the error
      // policy. Two properties follow from that shape:
      //
      //   - the pointer targets a heap object owned by the router, so its
      //     address is unaffected by the router being moved, and the
      //     handler stays callable on the router that received the move;
      //   - handler plus pointer is 16 bytes, which fits move_only_fn's
      //     inline buffer on every toolchain: it stores any closure up
      //     to 4 words that is nothrow-move-constructible — trivial
      //     copyability is not required, unlike libstdc++'s
      //     std::function. A shared_ptr capture would exceed the buffer
      //     and cost one heap allocation at registration.
      auto* error_policy = ensure_param_error().get();
      return [handler = std::move(handler), error_policy](Req& req, Res& res, const capture_list& captures) -> Task {
        auto resolved = resolve_params<params>(captures, req.context());
        if (resolved.error != param_error::none)
        {
          if (*error_policy)
            co_await (*error_policy)(req, res, resolved.error);
          co_return;
        }

        co_await std::apply(
          [&](auto&&... args) { return handler(req, res, std::forward<decltype(args)>(args)...); },
          std::move(*resolved.values)
        );
      };
    }

  private:
    struct node
    {
      // Keys and parameter names point into the pattern literal, which has
      // static storage duration — safe because a pattern can only come
      // from a string literal.
      std::unordered_map<std::string_view, std::unique_ptr<node>> literals;
      std::unique_ptr<node> param;
      std::string_view param_name;
      std::unique_ptr<node> catch_all;
      std::string_view catch_all_name;
      std::string_view pattern;
      handler_fn handler;
    };

    struct segment_list
    {
      std::array<std::string_view, max_segments> items{};
      // Offset of each segment's first character in the original path —
      // the catch-all edge captures the raw remainder from it, slashes
      // included.
      std::array<size_t, max_segments> offsets{};
      size_t count = 0;
      // Set when the path holds more segments than the list can store.
      // Without it a truncated path is indistinguishable from a short
      // one, and a pattern that ran out of segments exactly where the
      // list does would match a path it never finished consuming.
      bool truncated = false;

      void push(std::string_view value, size_t offset)
      {
        if (count < max_segments)
        {
          items[count] = value;
          offsets[count] = offset;
          ++count;
        }
        else
        {
          truncated = true;
        }
      }

      std::string_view operator[](size_t index) const
      {
        return items[index];
      }

      size_t offset(size_t index) const
      {
        return offsets[index];
      }

      size_t size() const
      {
        return count;
      }
    };

    template<typename Handler>
    void insert(std::string_view method, const typed_pattern<Handler>& pattern, handler_fn handler)
    {
      node* current = &roots_[std::string(method)];
      for (size_t i = 0; i < pattern.segment_count_; ++i)
      {
        const auto segment = pattern.segments_[i];
        if (segment[0] == ':')
        {
          if (!current->param)
            current->param = std::make_unique<node>();
          current->param->param_name = segment.substr(1);
          current = current->param.get();
        }
        else if (segment[0] == '*')
        {
          // The consteval validator guarantees a catch-all is the last
          // segment, so this node is always a leaf.
          if (!current->catch_all)
            current->catch_all = std::make_unique<node>();
          current->catch_all->catch_all_name = segment.substr(1);
          current = current->catch_all.get();
        }
        else
        {
          auto& child = current->literals[segment];
          if (!child)
            child = std::make_unique<node>();
          current = child.get();
        }
      }
      current->pattern = pattern.text_;
      current->handler = std::move(handler);
    }

    // Literal edges before the parameter edge, the catch-all edge last;
    // backtracking on failure. A catch-all consumes the rest of the path
    // (slashes included) and is always the pattern's last segment, so it
    // needs no backtracking of its own. It matches one or more remaining
    // segments — "/a/*x" does not match "/a".
    //
    // A pattern without a catch-all matches only a path of exactly its
    // own length: running out of segments together is a match, running
    // out of pattern while the path continues is not.
    // Literal-edge lookup. The raw segment is tried first — the common
    // case needs no decoding and is bit-for-bit the raw-only path. Only
    // a miss on a segment containing '%' retries with the decoded
    // form, so a pattern written in plain text matches both wire forms
    // of an escaped segment. The identity decoder never retries:
    // decoding is the identity, so the retry could not find anything
    // the first lookup missed.
    const node* find_literal(const node* current, std::string_view segment, [[maybe_unused]] std::span<char> scratch) const
    {
      if (const auto it = current->literals.find(segment); it != current->literals.end())
        return it->second.get();
      if constexpr (!std::is_same_v<Decoder, identity_segment_decoder>)
      {
        if (segment.find('%') == std::string_view::npos)
          return nullptr;
        const auto decoded_segment = decoder_.decode(segment, scratch.data(), scratch.size());
        if (!decoded_segment)
          return nullptr;
        if (const auto it = current->literals.find(*decoded_segment); it != current->literals.end())
          return it->second.get();
      }
      return nullptr;
    }

    // Decodes the matched captures through the segment decoder into the
    // caller's arena: values without '%' pass through as raw views (no
    // copy), values with '%' decode into the arena. Returns false when
    // a value cannot decode — malformed input, or the arena is too
    // small — which handle() reports as bad_path.
    bool decode_captures(const capture_list& raw, capture_list& decoded, std::span<char> arena) const
    {
      size_t used = 0;
      for (size_t i = 0; i < raw.size(); ++i)
      {
        const capture& c = raw[i];
        if (c.value.find('%') == std::string_view::npos)
        {
          decoded.push(c);
          continue;
        }
        const auto decoded_value = decoder_.decode(c.value, arena.data() + used, arena.size() - used);
        if (!decoded_value)
          return false;
        decoded.push(capture{c.name, *decoded_value});
        used += decoded_value->size();
      }
      return true;
    }

    const node* walk(
      const node* current, const segment_list& segments, size_t index, std::string_view path,
      capture_list& captures, std::span<char> scratch) const
    {
      if (index == segments.size())
      {
        // A truncated list means the path continues past what the walk
        // could see, so the pattern is exhausted first. A catch-all
        // would have taken the remainder at an earlier index, so there
        // is nothing left to try here.
        if (segments.truncated)
          return nullptr;
        return current->handler ? current : nullptr;
      }

      if (const node* child = find_literal(current, segments[index], scratch))
      {
        if (const node* hit = walk(child, segments, index + 1, path, captures, scratch))
          return hit;
      }
      if (current->param)
      {
        captures.push(capture{current->param->param_name, segments[index]});
        if (const node* hit = walk(current->param.get(), segments, index + 1, path, captures, scratch))
          return hit;
        captures.pop();
      }
      if (current->catch_all)
      {
        captures.push(capture{current->catch_all->catch_all_name, path.substr(segments.offset(index))});
        if (current->catch_all->handler)
          return current->catch_all.get();
        captures.pop();
      }
      return nullptr;
    }

    // Request paths, unlike patterns, tolerate empty segments: "//a//b"
    // splits like "/a/b". Client input is parsed leniently; developer
    // patterns are validated strictly at compile time.
    static segment_list split_path(std::string_view path)
    {
      segment_list segments;
      size_t start = 0;
      while (start < path.size())
      {
        if (path[start] == '/')
        {
          ++start;
          continue;
        }
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
          end = path.size();
        segments.push(path.substr(start, end - start), start);
        start = end;
      }
      return segments;
    }

    // Method keys are owned strings: route() accepts any string_view at
    // registration, and the transparent hash/equal let match() look up by
    // string_view without allocating.
    // The segment decoder. Stateless by convention (default-constructed
    // and no_unique_address, so an empty decoder costs nothing); the
    // router calls only its const decode().
    [[no_unique_address]] Decoder decoder_{};
    std::unordered_map<std::string, node, detail::string_hash, detail::string_equal> roots_;
    not_found_fn not_found_;
    method_not_allowed_fn method_not_allowed_;
    // Heap-allocated so that its address is stable: bound handlers hold a
    // raw pointer to it (see bind_route), and that pointer has to survive
    // the router being moved. A moved-from router leaves this null;
    // ensure_param_error re-allocates.
    std::unique_ptr<param_error_fn> param_error_ = std::make_unique<param_error_fn>();
    std::shared_ptr<http::context> app_context_;

    // The composed onion: chain(req, res, terminal, info) runs
    // middleware 0..n-1 around terminal. Rebuilt once per use() call
    // (startup), not per request; the per-request terminal is the
    // matched handler (or the not-found handler). shared_ptr so the
    // nested next-callables stay valid even if the router is moved.
    using chain_fn = move_only_fn<Task(Req&, Res&, const next_fn&, const route_info&)>;
    // Held via shared_ptr so rebuild_pipeline can compose the chain from
    // every registered middleware without copying the move-only
    // middleware_fn objects.
    std::vector<std::shared_ptr<middleware_fn>> middlewares_;
    std::shared_ptr<chain_fn> pipeline_;

    // The policy object every bound handler points at. A moved-from
    // router has none; re-allocate on demand so that registering on a
    // moved-from router still yields handlers that observe later updates.
    const std::unique_ptr<param_error_fn>& ensure_param_error()
    {
      if (!param_error_)
        param_error_ = std::make_unique<param_error_fn>();
      return param_error_;
    }

    void rebuild_pipeline()
    {
      auto chain = std::make_shared<chain_fn>(
        [](Req&, Res&, const next_fn& terminal, const route_info&) -> Task { co_await terminal(); });
      for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it)
      {
        auto mw = *it;
        auto inner = std::move(chain);
        chain = std::make_shared<chain_fn>(
          [mw = std::move(mw), inner = std::move(inner)](Req& req, Res& res, const next_fn& terminal, const route_info& info) -> Task {
            // The inner next captures a raw chain_fn* (not the shared_ptr:
            // 16 bytes) so the closure is 5 pointers = 40 bytes and stays
            // in move_only_fn's inline buffer — no heap allocation per
            // request per middleware. The outer closure's shared_ptr
            // keeps the chain_fn alive for the whole request.
            co_await (*mw)(req, res, [inner = inner.get(), &req, &res, &terminal, &info]() -> Task { co_await (*inner)(req, res, terminal, info); }, info);
          });
      }
      pipeline_ = std::move(chain);
    }
  };
} // namespace http

#endif // HTTPROUTE20_ROUTER_H
