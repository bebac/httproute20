// Compile-fail test: a middleware must be callable as
// Task(Req&, Res&, next) or Task(Req&, Res&, next, const route_info&)
// — this one returns int and takes no next, so router::use rejects it
// via the middleware_like concept (see tests/CMakeLists.txt for the
// expected diagnostic).
#include "../support.h"

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.use([](httptest::test_request&, httptest::test_response&) { return 0; });
  return 0;
}
