// Compile-fail test: a router's fourth template parameter must satisfy
// segment_decoder — this one has no decode(), so the router template
// rejects it (see tests/CMakeLists.txt for the expected diagnostic).
#include "../support.h"

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  http::router<httptest::test_task, httptest::test_request, httptest::test_response, int> router;
  (void)router;
  return 0;
}