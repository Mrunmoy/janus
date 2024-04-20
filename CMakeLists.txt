cmake_minimum_required(VERSION 3.16)

project(cfuture
    VERSION 0.1.0
    DESCRIPTION "Zero-Heap Lock-Free Future/Promise Framework for Embedded C"
    LANGUAGES C CXX
)

# Standard options
option(CFUTURE_BUILD_TESTS "Build unit test suite" ON)
option(CFUTURE_BUILD_BENCHMARKS "Build micro-benchmarks" ON)
option(CFUTURE_BUILD_EXAMPLES "Build example applications" ON)
option(CFUTURE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(CFUTURE_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CFUTURE_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

# Determine if building as standalone project
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CFUTURE_IS_ROOT_PROJECT ON)
else()
    set(CFUTURE_IS_ROOT_PROJECT OFF)
    set(CFUTURE_BUILD_TESTS OFF)
    set(CFUTURE_BUILD_BENCHMARKS OFF)
    set(CFUTURE_BUILD_EXAMPLES OFF)
endif()

# Sanitizer options
if(CFUTURE_ENABLE_TSAN)
    message(STATUS "Enabling ThreadSanitizer (-fsanitize=thread)")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

if(CFUTURE_ENABLE_ASAN)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CFUTURE_ENABLE_COVERAGE)
    message(STATUS "Enabling code coverage instrumentation")
    add_compile_options(--coverage -O1 -g -Wno-error)
    add_link_options(--coverage)
endif()

# Platform-specific sources
set(CFUTURE_PLATFORM_SOURCES
    src/adapters/cfuture_polling.c
)

if(WIN32)
    list(APPEND CFUTURE_PLATFORM_SOURCES src/adapters/cfuture_win32.c)
else()
    list(APPEND CFUTURE_PLATFORM_SOURCES src/adapters/cfuture_posix.c)
    find_package(Threads REQUIRED)
endif()

# Core library
add_library(cfuture STATIC
    src/cfuture.c
    ${CFUTURE_PLATFORM_SOURCES}
)

target_include_directories(cfuture
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(cfuture PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# Strict warning flags
if(MSVC)
    target_compile_options(cfuture PRIVATE /W4 /WX)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cfuture PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -Wshadow
        -Wundef
        -Wstrict-prototypes
        -Wpointer-arith
        -Wcast-align
    )
endif()

# Threading library dependency (pthread for posix adapter on Unix/macOS)
if(NOT WIN32)
    target_link_libraries(cfuture PUBLIC Threads::Threads)
endif()

# Custom verification targets
add_custom_target(size-check
    COMMAND ${CMAKE_COMMAND} -E echo "--- libcfuture memory footprint ---"
    COMMAND size $<TARGET_FILE:cfuture>
    DEPENDS cfuture
    COMMENT "Reporting binary footprint of libcfuture"
)

add_custom_target(zero-heap-check
    COMMAND ${CMAKE_COMMAND} -E echo "--- Verifying zero dynamic memory symbols ---"
    COMMAND bash -c "! nm $<TARGET_FILE:cfuture> | grep -E '(malloc|free|calloc|realloc)' || (echo 'ERROR: Dynamic allocation detected!' && exit 1)"
    DEPENDS cfuture
    COMMENT "Auditing libcfuture symbols for zero heap allocation"
)

# Subdirectories
if(CFUTURE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CFUTURE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(CFUTURE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
