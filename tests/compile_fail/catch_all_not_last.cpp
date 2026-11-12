// Compile-fail test: a "*name" catch-all consumes the rest of the path,
// so it is only valid as the last segment. "/a/*x/b" is rejected by the
// consteval pattern validator (see tests/CMakeLists.txt for the expected
// diagnostic).
#include "../support.h"

namespace
{
  httptest::test_task tail_handler(httptest::test_request&, httptest::test_response&, std::string)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/a/*x/b", &tail_handler);
  return 0;
}
