// Compile-fail test: the pattern has one ":name" segment but the handler
// has two path parameters (unsigned and std::string) plus an injected one,
// so typed_pattern's consteval arity check rejects the registration — the
// ":name" count is compared against the path parameter count only (see
// tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

namespace
{
  struct svc
  {
  };

  httptest::test_task two_path_params(httptest::test_request&, httptest::test_response&, unsigned, std::string, svc&)
  {
    co_return;
  }
} // namespace

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/entity/:id", &two_path_params);
  return 0;
}