add_executable(minidb_bench bench.c profiler.c)
target_link_libraries(minidb_bench PRIVATE minidb_core)

# The built-in profiler records raw return addresses. A static, non-PIE binary
# keeps every address (libc included) fixed, so addr2line can name them.
option(MINIDB_STATIC_BENCH "Link minidb_bench statically for --profile" OFF)
if(MINIDB_STATIC_BENCH)
    target_link_options(minidb_bench PRIVATE -static)
endif()
