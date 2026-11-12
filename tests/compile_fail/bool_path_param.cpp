// Compile-fail test: bool is not a path parameter (std::from_chars has
// no bool overload), so the handler's bool parameter is not counted as
// one. The pattern's ':value' segment therefore has no matching path
// parameter and the arity check in typed_pattern's consteval
// constructor fails (see tests/CMakeLists.txt for the expected
// diagnostic). Before the bool exclusion this compiled past the arity
// check and failed deep inside bind_route, in from_string<bool>::parse.
#include "../support.h"

namespace
{
  httptest::test_task bool_handler(httptest::test_request&, httptest::test_response&, bool)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/flag/:value", &bool_handler);
  return 0;
}