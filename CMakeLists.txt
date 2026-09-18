cmake_minimum_required(VERSION 3.16)
project(circular_queue LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cq INTERFACE)
target_include_directories(cq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(cq INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

enable_testing()

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(cq_tests
    tests/crc32_test.cpp
    tests/circular_queue_test.cpp
    tests/concurrency_test.cpp
)
target_compile_definitions(cq_tests PRIVATE CQ_ENABLE_TEST_HOOKS)
target_link_libraries(cq_tests PRIVATE cq GTest::gtest_main Threads::Threads)
target_compile_options(cq_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

include(GoogleTest)
gtest_discover_tests(cq_tests)

add_executable(cq_demo examples/demo_main.cpp)
target_link_libraries(cq_demo PRIVATE cq Threads::Threads)
target_compile_options(cq_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
