# The engine is one static library; the CLI, tests and benchmarks link against it.
add_library(minidb_core
    common/status.c
    common/arena.c
    value/value.c
    schema/schema.c
    row/row.c
)

add_executable(minidb main.c)
target_link_libraries(minidb PRIVATE minidb_core)
