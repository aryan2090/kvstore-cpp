cmake_minimum_required(VERSION 3.20)
project(kvstore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(KV_SANITIZE "" CACHE STRING "Sanitizer build: thread, or address (address + undefined)")

add_compile_options(-Wall -Wextra -Werror)
if(KV_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(KV_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(kv src/crc32.cpp src/record.cpp src/file.cpp src/db.cpp)
target_include_directories(kv PUBLIC include)
target_link_libraries(kv PUBLIC Threads::Threads)

add_executable(kvcli tools/kvcli.cpp)
target_link_libraries(kvcli PRIVATE kv)

add_executable(kvbench bench/kvbench.cpp)
target_link_libraries(kvbench PRIVATE kv)

option(KV_BUILD_TESTS "Build tests" ON)
if(KV_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(kv_tests tests/record_test.cpp tests/db_test.cpp tests/recovery_test.cpp
                          tests/compaction_test.cpp tests/concurrency_test.cpp)
  target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(kv_tests DISCOVERY_TIMEOUT 60)
endif()
