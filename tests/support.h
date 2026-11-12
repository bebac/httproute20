#ifndef HTTPPROUTE20_TESTS_SUPPORT_H
#define HTTPPROUTE20_TESTS_SUPPORT_H

#include <http/router.h>

#include <coroutine>
#include <cstddef>
#include <exception>
#include <new> // declares the global operator new/delete used below
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httptest
{
  // A minimal freely-awaitable coroutine task: satisfies task_like with
  // no runtime behind it. A coroutine returning test_task runs eagerly
  // to completion when called, so tests can invoke bound handlers
  // directly and observe their side effects.
  struct test_task
  {
    struct promise_type
    {
      test_task get_return_object()
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

  // A test task whose coroutine frames are carved from a fixed arena
  // instead of the heap, so the allocation counter (main.cpp) measures
  // only the router's own allocations, not coroutine frames. Safe here
  // because every coroutine in the tests runs eagerly to completion
  // (suspend_never initial and final suspend): frames are taken and
  // released in LIFO order within one synchronous call, and no frame is
  // alive across a suspension.
  struct arena_task
  {
    struct promise_type
    {
      static constexpr size_t frame_size = 4096;
      static constexpr size_t frame_count = 64;
      alignas(std::max_align_t) inline static std::byte arena_[frame_count * frame_size];
      inline static size_t top_ = 0;

      static void* operator new(std::size_t size)
      {
        if (size <= frame_size && top_ < frame_count)
          return arena_ + (top_++) * frame_size;
        return ::operator new(size); // oversized frame: heap, counted
      }

      static void operator delete(void* p, std::size_t size) noexcept
      {
        if (top_ > 0 && p == static_cast<void*>(arena_ + (top_ - 1) * frame_size))
        {
          --top_; // LIFO: the most recently taken frame is released first
          return;
        }
        // Unsized form: the sized ::operator delete(void*, size_t) is
        // declared only when __cpp_sized_deallocation is defined, which
        // is not guaranteed (clang with libstdc++ rejects the sized
        // call). The size is only a deallocation hint, so drop it.
        ::operator delete(p);
      }

      static void operator delete(void* p) noexcept
      {
        if (top_ > 0 && p == static_cast<void*>(arena_ + (top_ - 1) * frame_size))
        {
          --top_;
          return;
        }
        ::operator delete(p);
      }

      arena_task get_return_object()
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

  // A request double: the router reads only context() from a request.
  // The method is the consumer's own addition — request_like does not
  // require it, but a 405/OPTIONS handler needs it, so the double has it.
  struct test_request
  {
    http::context* ctx = nullptr;
    std::string_view method = {};

    http::context* context() const
    {
      return ctx;
    }
  };

  // A response double: the setters are the response_like contract, the
  // getters the response_readable one.
  struct test_response
  {
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers_;

    test_response& status(int code)
    {
      status_code = code;
      return *this;
    }

    int status() const
    {
      return status_code;
    }

    test_response& header(std::string name, std::string value)
    {
      headers_[std::move(name)] = std::move(value);
      return *this;
    }

    std::optional<std::string_view> header(std::string_view name) const
    {
      if (const auto it = headers_.find(std::string(name)); it != headers_.end())
        return std::string_view{it->second};
      return std::nullopt;
    }
  };

  // The library bound to the test doubles.
  using router = http::router<test_task, test_request, test_response>;

  // Heap-allocation accounting. The test binary replaces the global
  // operator new (tests/main.cpp), so the router's allocation behaviour
  // is observable. match() is documented as allocation-free, and that is
  // easy to break without noticing: a bound closure that outgrows
  // std::function's inline buffer, or that stops being trivially
  // copyable, silently costs one allocation per request while every
  // functional test stays green.
  //
  // Counting is off by default and only on inside an allocation_scope,
  // so doctest's own allocations never contaminate a measurement.
  inline bool counting_allocations = false;
  inline long allocation_count = 0;

  class allocation_scope
  {
  public:
    allocation_scope() noexcept
    {
      allocation_count = 0;
      counting_allocations = true;
    }

    ~allocation_scope() noexcept
    {
      counting_allocations = false;
    }

    allocation_scope(const allocation_scope&) = delete;
    allocation_scope& operator=(const allocation_scope&) = delete;

    long count() const noexcept
    {
      return allocation_count;
    }
  };
} // namespace httptest

#endif // HTTPPROUTE20_TESTS_SUPPORT_H
