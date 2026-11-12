cmake_minimum_required(VERSION 3.20)
project(httproute20 VERSION 0.1.0 LANGUAGES CXX)

# Header-only: everything lives in include/.
add_library(httproute20 INTERFACE)
add_library(httproute20::httproute20 ALIAS httproute20)

target_include_directories(httproute20 INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(httproute20 INTERFACE cxx_std_20)

# A "local build" is httproute20 as the top-level project. A vendored
# add_subdirectory() build compiles only the header-only interface
# target: no tests, no sanitizer, nothing imposed on the consumer.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(HTTPROUTE20_LOCAL_BUILD ON)
else()
  set(HTTPROUTE20_LOCAL_BUILD OFF)
endif()

# AddressSanitizer for the test suite: on by default for local builds,
# off when vendored. It is applied per test executable (see
# tests/CMakeLists.txt) and never to the INTERFACE library, so it cannot
# leak into a consumer's build. Turn it off with
# -DHTTPROUTE20_SANITIZE=OFF.
option(HTTPROUTE20_SANITIZE
  "Build the test suite with AddressSanitizer"
  ${HTTPROUTE20_LOCAL_BUILD}
)

if(HTTPROUTE20_LOCAL_BUILD)
  include(FetchContent)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
  )
  FetchContent_MakeAvailable(doctest)
  enable_testing()
  add_subdirectory(tests)
endif()