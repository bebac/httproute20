// Compile-fail test: the template-argument pattern syntax
// router.get<"/entity/:id">(&wrong_handler) makes the pattern a constant
// expression (a structural NTTP), so the arity check is a static_assert
// that names both counts — unlike the literal syntax, whose check
// degrades to a consteval throw (see arity_mismatch.cpp). The handler has
// two trailing parameters but the pattern has one ':name' segment (see
// tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

namespace
{
  httptest::test_task wrong_handler(httptest::test_request&, httptest::test_response&, int, int)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get<"/entity/:id">(&wrong_handler);
  return 0;
}
