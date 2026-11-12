// Compile-fail test: "//" is an empty segment, which the splitter would
// silently skip — making "/entity//latest" the same route as
// "/entity/latest". The consteval pattern validator rejects it (see
// tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

namespace
{
  httptest::test_task latest_handler(httptest::test_request&, httptest::test_response&)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity//latest", &latest_handler);
  return 0;
}