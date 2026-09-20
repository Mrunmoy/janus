# Benchmarks
add_executable(bench_throughput bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE cfuture)
target_compile_features(bench_throughput PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(bench_throughput PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
