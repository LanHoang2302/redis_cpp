cmake_minimum_required(VERSION 3.20)
project(redis_cpp CXX)

# ─── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─── Build type defaults ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ─── Compiler flags ───────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)
add_compile_options("$<$<CONFIG:Release>:-O3>")
add_compile_options("$<$<CONFIG:Release>:-march=native>")
add_compile_options("$<$<CONFIG:Debug>:-g>")
add_compile_options("$<$<CONFIG:Debug>:-O0>")
add_compile_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")

add_link_options(
    "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
)

# ─── Source library (shared between server and tests) ─────────────────────────
add_library(redis_cpp_lib STATIC
    src/net/Socket.hpp                  # header-only
    src/net/Connection.hpp              # header-only
    src/net/EpollReactor.cpp
    src/protocol/RespParser.cpp
    src/store/TtlManager.cpp
    src/store/AofWriter.cpp
    src/store/KvStore.cpp
    src/commands/CommandDispatcher.cpp
    src/ThreadPool.hpp                  # header-only
)

target_include_directories(redis_cpp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_cpp_lib
    pthread
)

# ─── Server executable ────────────────────────────────────────────────────────
add_executable(redis_cpp
    src/main.cpp
)

target_link_libraries(redis_cpp
    redis_cpp_lib
)

# ─── Google Test (via FetchContent) ──────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent GoogleTest from overriding our compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ─── Tests ────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(run_tests
    tests/test_kv_store.cpp
    tests/test_resp_parser.cpp
    tests/test_ttl_manager.cpp
    tests/test_command_dispatcher.cpp
)

target_link_libraries(run_tests
    redis_cpp_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(run_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME IntegrationTests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_tests.py
)

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS redis_cpp DESTINATION bin)

# ─── Summary ─────────────────────────────────────────────────────────────────
message(STATUS "====================================================")
message(STATUS "  redis_cpp   — C++20 Redis-compatible KV server")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler  : ${CMAKE_CXX_COMPILER}")
message(STATUS "====================================================")
