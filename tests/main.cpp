#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "support.h"

#include <cstddef>
#include <cstdlib>
#include <new>

// Global allocation hooks for the allocation tests (see support.h).
// Replacing these is the portable way to observe how many heap
// allocations a piece of code performs. Everything is forwarded to
// std::malloc/std::free, so AddressSanitizer still tracks the memory --
// it intercepts malloc/free, not these operators.
//
// The array and sized forms are defined explicitly rather than left to
// forward to the scalar form, so that every allocation and deallocation
// goes through the same malloc/free pair and cannot mismatch.
void* operator new(std::size_t size)
{
  if (httptest::counting_allocations)
    ++httptest::allocation_count;
  if (void* memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
  return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
  std::free(memory);
}

void operator delete[](void* memory) noexcept
{
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
  std::free(memory);
}