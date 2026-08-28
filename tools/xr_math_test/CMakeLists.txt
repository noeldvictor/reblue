# Standalone, and deliberately not part of the main build.
#
# The main tree cannot configure without the ReXGlue SDK and cannot build
# without assets/default.xex. xr_math.h depends on nothing but the integer
# aliases, so it can be compiled and run on its own against the stub in
# stub/rex - which is what makes the handedness conversion checkable at all.
#
#   cmake -S tools/xr_math_test -B out/xr_math_test -G Ninja
#   cmake --build out/xr_math_test
#   ./out/xr_math_test/xr_math_test
cmake_minimum_required(VERSION 3.25)
project(xr_math_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(xr_math_test xr_math_test.cpp)

# stub/ first so <rex/types.h> resolves here rather than to a real SDK if one
# happens to be installed: the test is about xr_math.h, not about the SDK.
target_include_directories(xr_math_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stub"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(MSVC)
    target_compile_options(xr_math_test PRIVATE /W4)
else()
    target_compile_options(xr_math_test PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME xr_math COMMAND xr_math_test)
