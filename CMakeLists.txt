# Copyright (c) 2026 caomengxuan666
# windows-top is extracted from WinuxCmd and licensed under the MIT License.

cmake_minimum_required(VERSION 3.20)

project(windows-top
  VERSION 0.1.0
  DESCRIPTION "A native Windows implementation of the Unix top command"
  LANGUAGES CXX)

option(WINDOWS_TOP_BUILD_TESTS "Build tests" ON)

add_library(windows_top STATIC
  src/windows_top.cpp)

target_compile_features(windows_top PUBLIC cxx_std_17)
target_include_directories(windows_top
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(windows_top PRIVATE psapi)

if(MSVC)
  target_compile_options(windows_top PRIVATE /W4 /permissive-)
else()
  target_compile_options(windows_top PRIVATE -Wall -Wextra -pedantic)
endif()

add_executable(top src/main.cpp)
target_link_libraries(top PRIVATE windows_top)
set_target_properties(top PROPERTIES OUTPUT_NAME "top")

if(WINDOWS_TOP_BUILD_TESTS)
  enable_testing()

  add_executable(windows_top_api_smoke tests/api_smoke.cpp)
  target_link_libraries(windows_top_api_smoke PRIVATE windows_top)

  add_test(NAME api_smoke COMMAND windows_top_api_smoke)
  add_test(NAME top_help COMMAND top --help)
  add_test(NAME top_batch_once COMMAND top -b -n 1 -d 0.1)
  add_test(NAME top_batch_sort_mem COMMAND top -b -n 1 -d 0.1 -o MEM)
endif()
