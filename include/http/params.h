#ifndef HTTPROUTE20_PARAMS_H
#define HTTPROUTE20_PARAMS_H

#include "http/captures.h"
#include "http/context.h"

#include <charconv>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace http
{
  // Parameter resolution: from_string/path_parameter classify a handler
  // parameter as parsed from a path segment, injector/injectable as
  // resolved from the context, and resolve_params turns the captures the
  // matcher collected into the handler's argument tuple.
  // Conversion from a path segment to a handler parameter type. The
  // primary template is deliberately undefined: a type becomes a *path*
  // parameter by specializing from_string<T>; a type without a
  // specialization is an *injected* parameter instead, and the undefined
  // primary makes the classification below a substitution failure rather
  // than a hard error.
  //
  // The signature contract is checked by path_parameter below: parse
  // takes the segment by value and returns std::optional<T>, nullopt
  // when the segment does not parse. The semantic contract is unchecked:
  // parse is equality-preserving — the same text yields the same result —
  // and neither modifies its input nor has other side effects. A
  // specialization that breaks the semantics satisfies path_parameter
  // without modeling it: ill-formed, no diagnostic required.
  template<typename T>
  struct from_string;

  // Integral types are parsed strictly with std::from_chars: the whole
  // segment must be consumed. bool is excluded: std::from_chars has no
  // bool overload, and path_parameter below only checks parse's
  // signature — without the constraint bool would satisfy the concept
  // and fail to compile later, deep inside bind_route.
  template<std::integral T>
    requires(!std::same_as<T, bool>)
  struct from_string<T>
  {
    static std::optional<T> parse(std::string_view text)
    {
      T value = 0;
      const char* first = text.data();
      const char* last = text.data() + text.size();
      auto result = std::from_chars(first, last, value);
      if (result.ec != std::errc{} || result.ptr != last)
        return std::nullopt;
      return value;
    }
  };

  template<>
  struct from_string<std::string>
  {
    static std::optional<std::string> parse(std::string_view text)
    {
      return std::string(text);
    }
  };

  // Borrows from the request path — only valid while the request lives.
  template<>
  struct from_string<std::string_view>
  {
    static std::optional<std::string_view> parse(std::string_view text)
    {
      return text;
    }
  };

  // A type is a path parameter if from_string<T> can parse a segment into
  // it. With the undefined primary template this is a concept check:
  // non-path types fail by substitution, not by error.
  //
  // The return type must be exactly std::optional<T> (nullopt on
  // failure), not merely convertible to it: optional<T> is constructible
  // from T itself, so a parse returning T passes a convertible_to check
  // and then fails inside resolve_one on !value — or, for a
  // bool-convertible T, compiles and misreports a valid segment as
  // bad_path.
  //
  // The const-lvalue and rvalue calls pin parse's by-value parameter: a
  // parse(std::string_view&) overload would satisfy the lvalue expression
  // but not the const/rvalue variations a requires-expression implicitly
  // demands — satisfied but not modeled. Making them explicit turns
  // that into a substitution failure here.
  template<typename T>
  concept path_parameter = requires(std::string_view text, const std::string_view const_text) {
    { from_string<T>::parse(text) } -> std::same_as<std::optional<T>>;
    from_string<T>::parse(const_text);
    from_string<T>::parse(std::move(text));
  };

  // Resolves an injected handler parameter from the request context. The
  // primary template is a plain context lookup: any type registered with
  // context::set<T> is injectable as T& with no per-type code. Specialize
  // injector<T> when resolution is more than a lookup (e.g. a per-request
  // object derived from an app-level service).
  //
  // Semantic contract (unchecked): resolve is equality-preserving for a
  // given context state and does not modify the context.
  template<typename T>
  struct injector
  {
    static std::optional<std::reference_wrapper<T>> resolve(const context* ctx)
    {
      if (ctx == nullptr)
        return std::nullopt;
      if (T* found = ctx->get<T>())
        return std::ref(*found);
      return std::nullopt;
    }
  };

  // A type is injectable if injector<T>::resolve is well-formed and yields
  // a reference to T.
  template<typename T>
  concept injectable = requires(const context* ctx) {
    { injector<T>::resolve(ctx) } -> std::convertible_to<std::optional<std::reference_wrapper<T>>>;
  };

  // Per-parameter opt-in to injection: a handler parameter of type
  // injected<T> is always resolved from the request context as T&,
  // never parsed from a path segment — even when T has a from_string
  // specialization (e.g. injected<std::string> for a config value).
  // Plain parameters keep the default classification: parsed when
  // from_string<T> exists, injected otherwise.
  template<typename T>
  struct injected
  {
    T& value;
    operator T&() const noexcept
    {
      return value;
    }
  };

  namespace detail
  {
    template<typename T>
    struct is_injected : std::false_type
    {
    };

    template<typename T>
    struct is_injected<injected<T>> : std::true_type
    {
    };

    template<typename T>
    struct unwrap_injected
    {
      using type = T;
    };

    template<typename T>
    struct unwrap_injected<injected<T>>
    {
      using type = T;
    };

    template<typename T>
    using unwrap_injected_t = typename unwrap_injected<std::remove_cvref_t<T>>::type;

    // A parameter is parsed from a path segment iff it is not wrapped in
    // injected<> and its (unwrapped) type has a from_string specialization.
    template<typename P>
    concept parsed_param = !is_injected<std::remove_cvref_t<P>>::value && path_parameter<std::remove_cvref_t<P>>;
  } // namespace detail

  // A trailing handler parameter is either parsed from a ":name" segment
  // (path parameter) or injected from the context.
  template<typename T>
  concept route_param = detail::parsed_param<T> || injectable<detail::unwrap_injected_t<T>>;

  enum class param_error
  {
    none,
    bad_path,          // a ":name" segment failed to parse -> client error
    missing_injection, // an injected parameter is not in the context -> config error
  };

  // The handler's trailing parameter tuple, resolved: path parameters
  // parsed from the captures, injected parameters from the context.
  template<typename Tuple>
  struct resolved_params
  {
    param_error error = param_error::none;
    std::optional<Tuple> values;
  };

  namespace detail
  {
    // Parses capture I into the tuple element's cv-unqualified type,
    // mirroring resolve_one below.
    template<size_t I, typename Tuple>
    std::optional<std::remove_cvref_t<std::tuple_element_t<I, Tuple>>> parse_one(const capture_list& captures)
    {
      using param = std::remove_cvref_t<std::tuple_element_t<I, Tuple>>;
      return from_string<param>::parse(captures[I].value);
    }

    // The tuple is constructed from the parsed values rather than assigned
    // into, so cv-qualified elements (e.g. std::tuple<const unsigned>)
    // convert the same way they resolve through resolve_params below.
    template<typename Tuple, size_t... I>
    std::optional<Tuple> convert_impl(const capture_list& captures, std::index_sequence<I...>)
    {
      if (captures.size() != sizeof...(I))
        return std::nullopt;
      auto parsed = std::tuple{parse_one<I, Tuple>(captures)...};
      if ((!std::get<I>(parsed) || ...))
        return std::nullopt;
      return std::optional<Tuple>{std::in_place, std::move(*std::get<I>(parsed))...};
    }

    // Storage for one resolved parameter: the parsed value for a path
    // parameter, a reference for an injected one (an injected<T> wrapper
    // unwraps to a reference to T).
    template<typename P>
    using resolved_storage_t = std::conditional_t<
      parsed_param<P>,
      std::optional<unwrap_injected_t<P>>,
      std::optional<std::reference_wrapper<unwrap_injected_t<P>>>>;

    template<size_t I, typename Tuple>
    resolved_storage_t<std::tuple_element_t<I, Tuple>> resolve_one(
      const capture_list& captures,
      size_t& capture_index,
      const context* ctx,
      param_error& error
    )
    {
      using param = std::tuple_element_t<I, Tuple>;
      using T = unwrap_injected_t<param>;
      if constexpr (parsed_param<param>)
      {
        if (capture_index >= captures.size())
        {
          error = param_error::bad_path;
          return {};
        }
        auto value = from_string<T>::parse(captures[capture_index++].value);
        if (!value)
        {
          error = param_error::bad_path;
          return {};
        }
        return std::move(value);
      }
      else
      {
        auto resolved = injector<T>::resolve(ctx);
        if (!resolved)
        {
          // First-error-wins would make the client/config error choice
          // depend on the parameter order; instead a bad path segment
          // (client error) always outranks a missing injection (config
          // error).
          if (error == param_error::none)
            error = param_error::missing_injection;
          return {};
        }
        return resolved;
      }
    }

    // The i-th argument for constructing the handler's parameter tuple.
    template<size_t I, typename Tuple, typename Resolved>
    decltype(auto) tuple_arg(Resolved& resolved)
    {
      using param = std::tuple_element_t<I, Tuple>;
      if constexpr (parsed_param<param>)
        return std::move(*std::get<I>(resolved));
      else if constexpr (is_injected<std::remove_cvref_t<param>>::value)
        return injected<unwrap_injected_t<param>>{std::get<I>(resolved)->get()};
      else
        return std::get<I>(resolved)->get();
    }

    template<typename Tuple, size_t... I>
    std::optional<Tuple> build_params(
      const std::tuple<resolved_storage_t<std::tuple_element_t<I, Tuple>>...>& resolved,
      std::index_sequence<I...>
    )
    {
      return std::optional<Tuple>{std::in_place, tuple_arg<I, Tuple>(resolved)...};
    }

    template<typename Tuple, size_t... I>
    resolved_params<Tuple> resolve_params_impl(
      const capture_list& captures,
      const context* ctx,
      std::index_sequence<I...>
    )
    {
      resolved_params<Tuple> out;
      size_t capture_index = 0;
      auto resolved = std::tuple<resolved_storage_t<std::tuple_element_t<I, Tuple>>...>{
        resolve_one<I, Tuple>(captures, capture_index, ctx, out.error)...
      };
      if (out.error == param_error::none && capture_index != captures.size())
        out.error = param_error::bad_path;
      if (out.error != param_error::none)
        return out;
      // emplace, not assign: a tuple holding an injected<T> (a reference
      // member) is not assignable, only constructible.
      if (auto values = build_params<Tuple>(resolved, std::index_sequence<I...>{}))
        out.values.emplace(std::move(*values));
      return out;
    }
  } // namespace detail

  // Converts ordered captures to a path-parameter tuple through
  // from_string (the path-only half of resolve_params below). Returns
  // nullopt when the capture count differs or a segment fails to parse.
  template<typename Tuple>
  std::optional<Tuple> convert_captures(const capture_list& captures)
  {
    return detail::convert_impl<Tuple>(captures, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
  }

  // Resolves a handler's trailing parameters: path parameters are parsed
  // from the captures in pattern order, injected parameters through
  // injector<T>::resolve from the request context. On failure `error`
  // distinguishes a segment that failed to parse (bad_path, a client
  // error) from an injected parameter missing from the context
  // (missing_injection, a config error); when both fail, bad_path wins —
  // the client error is reported.
  template<typename Tuple>
  resolved_params<Tuple> resolve_params(const capture_list& captures, const context* ctx)
  {
    return detail::resolve_params_impl<Tuple>(captures, ctx, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
  }
} // namespace http

#endif // HTTPROUTE20_PARAMS_H
