// Compile-fail test: from_string<raw> is specialized with a parse that
// returns raw itself instead of std::optional<raw>. The loose
// path_parameter constraint (convertible_to<std::optional<T>>) accepted
// that, because optional<T> is constructible from T, so raw was
// classified as a path parameter and the failure surfaced later, deep
// inside resolve_one. With the strict same_as<std::optional<T>>
// constraint raw is not a path parameter, so the ':value' segment has no
// matching path parameter and the arity check in typed_pattern's
// consteval constructor fails (see tests/CMakeLists.txt for the expected
// diagnostic).
#include "../support.h"

namespace
{
  struct raw
  {
    std::string value;
  };

  httptest::test_task raw_handler(httptest::test_request&, httptest::test_response&, raw)
  {
    co_return;
  }
} // namespace

template<>
struct http::from_string<raw>
{
  static raw parse(std::string_view text)
  {
    return raw{std::string(text)};
  }
};

// A main() so that a regression (the file compiling after all) links and
// the test fails properly instead of passing via an unrelated link error.
int main()
{
  httptest::router router;
  router.get("/thing/:value", &raw_handler);
  return 0;
}
