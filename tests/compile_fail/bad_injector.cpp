// Compile-fail test: injector<svc> is specialized with a resolve() that
// does not yield a reference to svc, so the handler's injected parameter
// fails the routable_params concept at registration (see
// tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

namespace
{
  struct svc
  {
  };
} // namespace

template<>
struct http::injector<svc>
{
  static int resolve(const http::context*)
  {
    return 0;
  }
};

namespace
{
  httptest::test_task bad_handler(httptest::test_request&, httptest::test_response&, svc&)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/thing", &bad_handler);
  return 0;
}