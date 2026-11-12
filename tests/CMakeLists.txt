# AddressSanitizer, applied to one test executable at a time. The router
# stores callables that outlive registration (bound handlers in the trie,
# the composed middleware chain), so a dangling capture is a real failure
# mode rather than a theoretical one; ASan turns it into a hard error at
# the first request instead of silent corruption.
#
# Called per test target, never on the INTERFACE library, so the
# sanitizer cannot leak into a consumer's build. -g is repeated on
# purpose: ASan reports are useless without symbols, whatever
# CMAKE_BUILD_TYPE the developer picked.
function(httproute20_enable_asan target)
  if(NOT HTTPROUTE20_SANITIZE)
    return()
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE
      -fsanitize=address
      -fno-omit-frame-pointer
      -g
    )
    target_link_options(${target} PRIVATE -fsanitize=address)
  else()
    message(STATUS
      "httproute20: no AddressSanitizer flags for ${CMAKE_CXX_COMPILER_ID}; "
      "${target} builds without it")
  endif()
endfunction()

add_executable(httproute20_tests
  main.cpp
  test_router.cpp
)
# doctest 2.5.x forward-declares std::basic_ostream itself unless this is
# defined, which leaves the type incomplete in the test TUs. On MSVC,
# stringifying a CHECK operand (e.g. a string_view) then instantiates
# operator<<(ostream&, string_view) against the incomplete type and fails
# with C2027. With the switch, doctest includes the real <ostream>.
target_compile_definitions(httproute20_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_link_libraries(httproute20_tests PRIVATE
  httproute20::httproute20
  doctest::doctest
)
httproute20_enable_asan(httproute20_tests)

add_test(NAME httproute20_tests COMMAND httproute20_tests)

# Compile-fail tests: each source under compile_fail/ must fail to build,
# and the compiler output must contain the expected diagnostic. Three
# kinds of anchor are used:
#
#   - the name of the deepest unsatisfied concept (missing_context,
#     void_return, member_pointer). Concept names are the portable
#     choice: GCC and Clang also print the constraint expression, MSVC
#     prints only the concept name.
#   - the consteval rejection marker (arity_mismatch). The pattern is
#     runtime data, so the ":name" count check happens in typed_pattern's
#     consteval constructor and surfaces as a non-constant call rather
#     than a concept failure; "is not a constant expression" is the one
#     phrase GCC, Clang and MSVC all emit for it.
#   - the static_assert message for the template-argument pattern syntax
#     (arity_mismatch_nttp): there the pattern is a structural NTTP, so the
#     arity check is a static_assert naming both counts; the message text
#     is the portable anchor (Clang additionally prints the requirement
#     with both values).
#   - the name of the deepest unsatisfied concept for an injected
#     parameter whose injector<T> cannot resolve it (bad_injector) — same
#     portability argument as the handler-side checks above.
#
# The targets are EXCLUDE_FROM_ALL so the normal build stays green; the
# test itself builds the single target through the compile_fail_test.cmake
# runner (ctest's WILL_FAIL cannot check the failure *reason*, the runner
# asserts both conditions).
function(add_compile_fail_test name expected_error)
  add_executable(compile_fail_${name} EXCLUDE_FROM_ALL compile_fail/${name}.cpp)
  target_link_libraries(compile_fail_${name} PRIVATE httproute20::httproute20)
  add_test(
    NAME compile_fail_${name}
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_COMMAND=${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DTARGET=compile_fail_${name}
      "-DEXPECTED_ERROR=${expected_error}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/compile_fail_test.cmake
  )
endfunction()

add_compile_fail_test(arity_mismatch
  "is not a constant expression")
add_compile_fail_test(arity_mismatch_nttp
  "does not match the handler")
add_compile_fail_test(bool_path_param
  "is not a constant expression")
add_compile_fail_test(bad_from_string
  "is not a constant expression")
add_compile_fail_test(empty_segment
  "is not a constant expression")
add_compile_fail_test(trailing_slash
  "is not a constant expression")
add_compile_fail_test(member_pointer
  "non_member_pointer")
add_compile_fail_test(missing_context
  "request_response_handler")
add_compile_fail_test(bad_injector
  "routable_params")
add_compile_fail_test(mixed_arity
  "is not a constant expression")
add_compile_fail_test(injected_with_capture
  "is not a constant expression")
add_compile_fail_test(void_return
  "task_like")
add_compile_fail_test(catch_all_not_last
  "is not a constant expression")
add_compile_fail_test(empty_catch_all
  "is not a constant expression")
add_compile_fail_test(bad_middleware
  "middleware_like")