// Compile-fail test: the handler lacks the leading (request&, response&)
// parameters. Must fail the request_response_handler concept on the
// arity constraint (see tests/CMakeLists.txt for the expected
// diagnostic).
#include "../support.h"

namespace
{
  httptest::test_task no_context(unsigned)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity/:id", &no_context);
  return 0;
}