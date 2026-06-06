# Build the core as a library
add_library(minidb_core
    schema/schema.c
    expression/expression.c
    operator/operator.c
)

# The CLI executable
add_executable(minidb main.c)
target_link_libraries(minidb PRIVATE minidb_core)
