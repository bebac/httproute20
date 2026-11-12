// Compile-fail test: the handler has two trailing parameters but the
// pattern has one ':name' segment. Must fail the arity check in
// typed_pattern's consteval constructor (see tests/CMakeLists.txt for
// the expected diagnostic).
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
  router.get("/entity/:id", &wrong_handler);
  return 0;
}