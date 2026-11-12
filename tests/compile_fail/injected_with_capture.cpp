// Compile-fail test: an injected<T> parameter is never parsed from a
// path segment, so a pattern with a ":name" segment does not match the
// handler's path-parameter count — typed_pattern's consteval arity check
// rejects the registration (see tests/CMakeLists.txt for the expected
// diagnostic).
#include "../support.h"

namespace
{
  httptest::test_task injected_only(httptest::test_request&, httptest::test_response&, http::injected<std::string>)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity/:id", &injected_only);
  return 0;
}
