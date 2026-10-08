cmake_minimum_required(VERSION 3.20)
project(ondevice_assistant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# address, undefined or thread. They cannot be mixed: thread conflicts with address.
set(ASSIST_SANITIZE "" CACHE STRING "sanitizers to build with, for example address,undefined or thread")
option(ASSIST_BUILD_TESTS "build the unit tests" ON)
option(ASSIST_BUILD_BENCH "build the benchmarks" ON)
option(ASSIST_BUILD_FUZZ "build the libFuzzer targets (needs clang)" OFF)

find_package(Threads REQUIRED)

# set before any target exists, so every target gets it
if(ASSIST_SANITIZE)
  add_compile_options(-fsanitize=${ASSIST_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${ASSIST_SANITIZE})
endif()

set(ASSIST_SOURCES
  core/src/tokenizer.cpp
  core/src/mapped_file.cpp
  core/src/model.cpp
  core/src/calendar.cpp
  core/src/slots.cpp
  core/src/calc.cpp
  core/src/fuzzy.cpp
  core/src/redact.cpp
  core/src/engine.cpp
  core/src/json.cpp
  core/src/service.cpp
  core/src/stream.cpp
)

add_library(assist_core STATIC ${ASSIST_SOURCES})
target_include_directories(assist_core PUBLIC core/include)
target_link_libraries(assist_core PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(assist_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(assist_cli cli/main.cpp)
target_link_libraries(assist_cli PRIVATE assist_core)

if(ASSIST_BUILD_TESTS)
  enable_testing()
  add_executable(assist_tests
    tests/main.cpp
    tests/test_tokenizer.cpp
    tests/test_model.cpp
    tests/test_calendar.cpp
    tests/test_slots.cpp
    tests/test_calc.cpp
    tests/test_fuzzy.cpp
    tests/test_redact.cpp
    tests/test_lru.cpp
    tests/test_engine.cpp
    tests/test_json.cpp
    tests/test_concurrency.cpp
    tests/test_no_alloc.cpp
    tests/alloc_counter.cpp
  )
  target_link_libraries(assist_tests PRIVATE assist_core)
  target_include_directories(assist_tests PRIVATE third_party tests)
  target_compile_definitions(assist_tests PRIVATE ASSIST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  add_test(NAME unit COMMAND assist_tests)
endif()

if(ASSIST_BUILD_BENCH)
  add_executable(assist_bench bench/bench.cpp)
  target_link_libraries(assist_bench PRIVATE assist_core)
endif()
