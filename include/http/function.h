#ifndef HTTPROUTE20_FUNCTION_H
#define HTTPROUTE20_FUNCTION_H

#include <concepts>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace http
{
  // A minimal move-only type-erased callable: std::function's move-only
  // sibling. C++20 has no std::move_only_function (that arrived in
  // C++23), and std::function requires its target to be
  // copy-constructible, which rejects move-only handlers and
  // middleware. A closure that fits the inline buffer and is
  // nothrow-move-constructible is stored inline — no allocation;
  // anything larger goes to the heap. The buffer is 5 words because the
  // per-request continuation closures the router builds capture at most
  // 5 pointers = 40 bytes (the terminal in handle() captures 4; each
  // pipeline layer's inner next captures the inner chain pointer plus
  // req, res, terminal and route_info); a smaller buffer would
  // heap-allocate one per request per middleware.
  template<typename Sig>
  class move_only_fn;

  template<typename R, typename... Args>
  class move_only_fn<R(Args...)>
  {
    static constexpr size_t inline_capacity = 5 * sizeof(void*);

    enum class op
    {
      move,
      destroy
    };

  public:
    move_only_fn() noexcept = default;

    template<typename F>
      requires(
        !std::same_as<std::remove_cvref_t<F>, move_only_fn> && std::invocable<F&, Args...> &&
        std::convertible_to<std::invoke_result_t<F&, Args...>, R>
      )
    move_only_fn(F&& f)
    {
      emplace(std::forward<F>(f));
    }

    move_only_fn(move_only_fn&& other) noexcept
    {
      transfer(std::move(other));
    }

    move_only_fn& operator=(move_only_fn&& other) noexcept
    {
      if (this != &other)
      {
        reset();
        transfer(std::move(other));
      }
      return *this;
    }

    move_only_fn(const move_only_fn&) = delete;
    move_only_fn& operator=(const move_only_fn&) = delete;

    ~move_only_fn()
    {
      reset();
    }

    explicit operator bool() const noexcept
    {
      return invoke_ != nullptr;
    }

    // Const like std::function::operator(): the target is invoked
    // through a non-const path internally, so mutable lambdas work.
    R operator()(Args... args) const
    {
      return invoke_(const_cast<std::byte*>(buffer_), std::forward<Args>(args)...);
    }

  private:
    template<typename F>
    void emplace(F&& f)
    {
      using G = std::remove_cvref_t<F>;
      if constexpr (
        sizeof(G) <= inline_capacity && alignof(G) <= alignof(void*) && std::is_nothrow_move_constructible_v<G>
      )
      {
        new (buffer_) G(std::forward<F>(f));
        invoke_ = [](std::byte* b, Args... args) -> R {
          return std::invoke(*reinterpret_cast<G*>(b), std::forward<Args>(args)...);
        };
        manage_ = [](std::byte* b, op o, std::byte* dest) noexcept {
          G* g = reinterpret_cast<G*>(b);
          if (o == op::move)
            new (dest) G(std::move(*g));
          g->~G();
        };
      }
      else
      {
        auto* p = new G(std::forward<F>(f));
        new (buffer_) G*(p);
        invoke_ = [](std::byte* b, Args... args) -> R {
          return std::invoke(**reinterpret_cast<G**>(b), std::forward<Args>(args)...);
        };
        manage_ = [](std::byte* b, op o, std::byte* dest) noexcept {
          G*& slot = *reinterpret_cast<G**>(b);
          if (o == op::move)
          {
            new (dest) G*(slot);
            slot = nullptr;
          }
          else
          {
            delete slot;
            slot = nullptr;
          }
        };
      }
    }

    void transfer(move_only_fn&& other) noexcept
    {
      if (other.invoke_ != nullptr)
      {
        other.manage_(other.buffer_, op::move, buffer_);
        invoke_ = other.invoke_;
        manage_ = other.manage_;
        other.invoke_ = nullptr;
        other.manage_ = nullptr;
      }
    }

    void reset() noexcept
    {
      if (invoke_ != nullptr)
      {
        manage_(buffer_, op::destroy, nullptr);
        invoke_ = nullptr;
        manage_ = nullptr;
      }
    }

    alignas(void*) std::byte buffer_[inline_capacity]{};
    R (*invoke_)(std::byte*, Args...) = nullptr;
    void (*manage_)(std::byte*, op, std::byte*) noexcept = nullptr;
  };
} // namespace http

#endif // HTTPROUTE20_FUNCTION_H
