// Compile-fail test: the handler is a member function pointer. It
// resolves through handler_traits but is not callable without an
// object, so it must fail the non_member_pointer concept (see
// tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

namespace
{
  struct controller
  {
    httptest::test_task get(httptest::test_request&, httptest::test_response&, unsigned)
    {
      co_return;
    }
  };
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity/:id", &controller::get);
  return 0;
}