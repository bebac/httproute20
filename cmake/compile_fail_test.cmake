# Compile-fail test runner: builds a target that is expected to fail to
# compile, and passes only when the build fails AND the compiler output
# contains the expected diagnostic. Invoked from tests/CMakeLists.txt:
#
#   cmake -DBUILD_COMMAND=<...> -DBUILD_DIR=<...> -DTARGET=<...>
#         -DEXPECTED_ERROR=<regex> -P compile_fail_test.cmake

execute_process(
  COMMAND ${BUILD_COMMAND} --build ${BUILD_DIR} --target ${TARGET}
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
  RESULT_VARIABLE result
)

if(result EQUAL 0)
  message(FATAL_ERROR
    "compile-fail test '${TARGET}' built successfully — the compile-time guard no longer fires")
endif()

if(NOT output MATCHES "${EXPECTED_ERROR}")
  message(FATAL_ERROR
    "compile-fail test '${TARGET}' failed with an unexpected diagnostic "
    "(expected '${EXPECTED_ERROR}'):\n${output}")
endif()