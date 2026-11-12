// Compile-fail test: "*" alone has no name to capture into — the consteval
// pattern validator rejects it like an empty ":name" segment (see
// tests/CMakeLists.txt for the expected diagnostic).
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
  router.get("/a/*", &tail_handler);
  return 0;
}
