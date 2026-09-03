cmake_minimum_required(VERSION 3.16)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Common test include directory
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

function(add_cfuture_test test_name)
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name} PRIVATE cfuture GTest::gtest GTest::gtest_main)
    target_compile_features(${test_name} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_cfuture_test(test_pool_init test_pool_init.cpp)
add_cfuture_test(test_lifecycle test_lifecycle.cpp)
add_cfuture_test(test_timeouts test_timeouts.cpp)
add_cfuture_test(test_isr_safety test_isr_safety.cpp)
add_cfuture_test(test_typed_pool test_typed_pool.cpp)
add_cfuture_test(test_concurrency_stress test_concurrency_stress.cpp)
