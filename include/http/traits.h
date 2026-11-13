#ifndef HTTPROUTE20_TRAITS_H
#define HTTPROUTE20_TRAITS_H

#include "http/captures.h"
#include "http/context.h"
#include "http/function.h"
#include "http/params.h"

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace http
{
  // The shared concepts and traits every layer constrains on: task_like,
  // request_like, response_like/response_readable, next_fn, route_info,
  // middleware_like, and the handler-signature traits (handler_traits,
  // route_handler, request_response_handler, routable_params).
  namespace detail
  {
    // await_suspend's result, per the await-expression rules: void
    // (resume inline), bool (whether to suspend), or a coroutine handle
    // (resume that one). Any other return type makes co_await
    // ill-formed.
    template<typename R>
    concept suspend_result =
      std::same_as<R, void> ||
      std::same_as<R, bool> ||
      std::convertible_to<R, std::coroutine_handle<>>;

    // An awaiter the router can drive: await_ready, await_suspend
    // accepting a plain std::coroutine_handle<> — so the awaiter is
    // freely usable in any promise, not bound to one promise type —
    // and await_resume.
    template<typename A>
    concept awaiter_like = requires(A awaiter, std::coroutine_handle<> handle) {
      { awaiter.await_ready() } -> std::convertible_to<bool>;
      { awaiter.await_suspend(handle) } -> suspend_result;
      awaiter.await_resume();
    };
  } // namespace detail

  // A task type the router can use as its coroutine currency: a
  // coroutine return type (promise_type) that co_await accepts, with
  // the awaiter's await_suspend taking a plain std::coroutine_handle<> —
  // freely awaitable in any promise, not only inside one runtime's own
  // coroutines. co_await resolves the awaiter as a member
  // operator co_await, else a free operator co_await (found by ADL),
  // else the object itself; a promise's await_transform is not
  // modelled — the type must be awaitable directly. Coroutine types
  // whose awaiter is bound to one specific promise type do not model
  // this and need an adapter.
  template<typename T>
  concept task_like = requires {
    typename T::promise_type;
  } && (
    (requires { std::declval<T>().operator co_await(); } &&
     detail::awaiter_like<decltype(std::declval<T>().operator co_await())>)
    || (!requires { std::declval<T>().operator co_await(); } &&
        requires { operator co_await(std::declval<T>()); } &&
        detail::awaiter_like<decltype(operator co_await(std::declval<T>()))>)
    || (!requires { std::declval<T>().operator co_await(); } &&
        !requires { operator co_await(std::declval<T>()); } &&
        detail::awaiter_like<T>)
  );

  // A request type. The router reads exactly one thing from it: the
  // per-request context scope injected handler parameters resolve from.
  template<typename Req>
  concept request_like = requires(Req& req) {
    { req.context() } -> std::convertible_to<const context*>;
  };

  // A response type. The router forwards Res opaquely — handlers,
  // middleware and the error policy receive Res& and call whatever they
  // need — so the concept is the write contract handlers respond
  // through: status(int) and header(name, value). The getters are a
  // separate contract, response_readable below.
  template<typename Res>
  concept response_like = requires(Res res, std::string name, std::string value) {
    res.status(int{});
    res.header(std::move(name), std::move(value));
  };

  // The read contract for post-processing middleware, which inspects the
  // response after next() returns: status() and header(name) getters.
  // The router never calls these — static_assert response_readable<Res>
  // in middleware that needs them.
  template<typename Res>
  concept response_readable = requires(Res res, std::string_view header_name) {
    { res.status() } -> std::convertible_to<int>;
    { res.header(header_name) } -> std::convertible_to<std::optional<std::string_view>>;
  };

  // The "rest of the onion" continuation a middleware receives: a
  // Task() invocable that runs the next stage — ultimately the matched
  // handler, or the not-found handler. Move-only (move_only_fn): the
  // continuation is single-shot, so a middleware takes it by value and
  // the pipeline moves it in.
  template<typename Task>
  using next_fn = move_only_fn<Task()>;

  // What a request matched, passed to middleware as a fourth argument:
  // the route template (e.g. "/entity/:id") and the captures as the
  // handlers see them — decoded, when the router's segment decoder
  // decodes — so access logging and metrics can see the route rather
  // than the raw path. All members are non-owning views — into the
  // router's trie and into handle()'s frame — valid for the duration
  // of the dispatch.
  struct route_info
  {
    std::string_view pattern = {};
    std::span<const capture> captures = {};
    bool matched = false;
    bool method_not_allowed = false;
    std::span<const std::string_view> allowed_methods = {};
  };

  // A callable usable as middleware: invocable as
  // Task(Req&, Res&, next_fn<Task>) or Task(Req&, Res&, next_fn<Task>,
  // const route_info&) — the fourth argument is what the request
  // matched. next is moved in — the continuation is single-shot, so a
  // middleware takes it by value. use() wraps the 3-argument form.
  template<typename M, typename Task, typename Req, typename Res>
  concept middleware_like = requires(M m, Req& req, Res& res, next_fn<Task> next, const route_info& info) {
    { m(req, res, std::move(next)) } -> std::convertible_to<Task>;
  } || requires(M m, Req& req, Res& res, next_fn<Task> next, const route_info& info) {
    { m(req, res, std::move(next), info) } -> std::convertible_to<Task>;
  };

  namespace detail
  {
    // Extracts the argument tuple from a handler signature: free
    // functions, function pointers, and lambdas/functors through
    // operator(). The primary template is defined but empty, so probing
    // handler_traits<T>::args on an unsupported type is a substitution
    // failure rather than a hard error — the concepts below rely on that.
    template<typename T>
    struct handler_traits
    {
    };

    template<typename R, typename... Args>
    struct handler_traits<R (*)(Args...)>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    template<typename R, typename... Args>
    struct handler_traits<R (*)(Args...) noexcept>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    template<typename R, typename C, typename... Args>
    struct handler_traits<R (C::*)(Args...)>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    template<typename R, typename C, typename... Args>
    struct handler_traits<R (C::*)(Args...) noexcept>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    template<typename R, typename C, typename... Args>
    struct handler_traits<R (C::*)(Args...) const>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    template<typename R, typename C, typename... Args>
    struct handler_traits<R (C::*)(Args...) const noexcept>
    {
      using return_type = R;
      using args = std::tuple<Args...>;
    };

    // Lambdas and other functors, resolved through their operator()
    // member. Constrained so that types without an operator() fall back
    // to the empty primary template instead of erroring inside the base
    // clause.
    //
    // Limitation: &T::operator() needs a single non-template operator(),
    // so generic lambdas (a templated operator()) and functors with
    // overloaded call operators fail the constraint, fall back to the
    // empty primary and are rejected by route_handler — with a
    // "handler_traits<T>::return_type" diagnostic rather than a clear
    // message about the operator() shape.
    template<typename T>
    requires requires { &T::operator(); }
    struct handler_traits<T> : handler_traits<decltype(&T::operator())>
    {
    };

    // Drops the leading request& and response& from the argument tuple.
    template<typename Tuple, typename IndexSequence>
    struct drop_first_two;

    template<typename Tuple, size_t... I>
    struct drop_first_two<Tuple, std::index_sequence<I...>>
    {
      using type = std::tuple<std::tuple_element_t<I + 2, Tuple>...>;
    };

    // The handler's trailing parameter tuple — only valid when
    // request_response_handler<T, Req, Res> holds.
    template<typename T>
    using handler_params = typename drop_first_two<
      typename handler_traits<T>::args,
      std::make_index_sequence<std::tuple_size_v<typename handler_traits<T>::args> - 2>>::type;

    template<typename T>
    inline constexpr size_t handler_param_count_v = std::tuple_size_v<handler_params<T>>;

    // Number of trailing parameters that are path parameters (the rest are
    // injected). typed_pattern's consteval arity check compares the ":name"
    // count against this.
    template<typename Tuple, typename IndexSequence>
    struct path_count;

    template<typename Tuple, size_t... I>
    struct path_count<Tuple, std::index_sequence<I...>>
    {
      static constexpr size_t value =
        (size_t{parsed_param<std::tuple_element_t<I, Tuple>>} + ... + size_t{0});
    };

    template<typename T>
    inline constexpr size_t path_param_count_v =
      path_count<handler_params<T>, std::make_index_sequence<handler_param_count_v<T>>>::value;

    // True when every trailing parameter of the handler is a path
    // parameter or injectable.
    template<typename Tuple, typename IndexSequence>
    struct all_routable;

    template<typename Tuple, size_t... I>
    struct all_routable<Tuple, std::index_sequence<I...>>
      : std::bool_constant<(route_param<std::remove_cvref_t<std::tuple_element_t<I, Tuple>>> && ...)>
    {
    };

    template<typename T>
    concept routable_params =
      all_routable<handler_params<T>, std::make_index_sequence<handler_param_count_v<T>>>::value;

    // Not a member function pointer: those resolve through handler_traits
    // but are not callable without an object, so they are rejected at the
    // registration site instead of deep inside the wrapper's call
    // expression.
    template<typename T>
    concept non_member_pointer = !std::is_member_pointer_v<T>;

    // A type usable as a typed route handler: not a member function
    // pointer, with a resolvable call signature returning a task_like
    // type.
    template<typename T>
    concept route_handler =
      non_member_pointer<T> &&
      requires {
        typename handler_traits<T>::return_type;
        typename handler_traits<T>::args;
      } &&
      task_like<typename handler_traits<T>::return_type>;

    // A handler whose two leading parameters are (Req&, Res&).
    // Concept conjunctions short-circuit: tuple_element_t is only
    // substituted once the arity constraint has held, so no laziness
    // guards are needed.
    template<typename T, typename Req, typename Res>
    concept request_response_handler =
      route_handler<T> &&
      std::tuple_size_v<typename handler_traits<T>::args> >= 2 &&
      std::same_as<std::tuple_element_t<0, typename handler_traits<T>::args>, Req&> &&
      std::same_as<std::tuple_element_t<1, typename handler_traits<T>::args>, Res&>;
  } // namespace detail
} // namespace http

#endif // HTTPROUTE20_TRAITS_H
