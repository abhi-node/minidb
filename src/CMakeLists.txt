# The engine is one static library; the CLI, tests and benchmarks link against it.
add_library(minidb_core
    common/status.c
)

add_executable(minidb main.c)
target_link_libraries(minidb PRIVATE minidb_core)
