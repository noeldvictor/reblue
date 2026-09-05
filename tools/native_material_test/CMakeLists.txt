cmake_minimum_required(VERSION 3.25)
project(native_material_test LANGUAGES CXX)
add_library(native_material_format STATIC
  ../../src/gpu/scene/native_material_data.cpp
  ../../src/gpu/scene/native_material_asset.cpp
  ../../src/gpu/scene/native_material_library.cpp)
target_include_directories(native_material_format PUBLIC ../../src)
target_compile_features(native_material_format PUBLIC cxx_std_23)
add_executable(native_material_test main.cpp assets.cpp skin.cpp)
target_link_libraries(native_material_test PRIVATE native_material_format)
add_executable(native_material_cook ../native_material_cook/main.cpp)
target_link_libraries(native_material_cook PRIVATE native_material_format)
enable_testing()
add_test(NAME native_material_data COMMAND native_material_test)
