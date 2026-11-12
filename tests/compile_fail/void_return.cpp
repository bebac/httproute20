// Compile-fail test: the handler returns void instead of a task_like
// type. Must fail the route_handler concept on the task_like
// return-type constraint (see tests/CMakeLists.txt for the expected
// diagnostic).
#include "../support.h"

namespace
{
  void void_handler(httptest::test_request&, httptest::test_response&, unsigned)
  {
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity/:id", &void_handler);
  return 0;
}