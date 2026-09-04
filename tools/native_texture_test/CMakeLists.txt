cmake_minimum_required(VERSION 3.25)
project(native_texture_test LANGUAGES CXX)
add_library(native_texture_format STATIC
  ../../src/gpu/scene/native_texture_data.cpp
  ../../src/gpu/scene/native_texture_library.cpp
  ../../src/gpu/host_mips.cpp)
target_include_directories(native_texture_format PUBLIC ../../src PRIVATE ../../thirdparty/stb)
target_compile_features(native_texture_format PUBLIC cxx_std_23)
add_executable(native_texture_test main.cpp)
target_link_libraries(native_texture_test PRIVATE native_texture_format)
add_executable(native_texture_cook ../native_texture_cook/main.cpp)
target_link_libraries(native_texture_cook PRIVATE native_texture_format)
enable_testing()
add_test(NAME native_texture_assets COMMAND native_texture_test)
