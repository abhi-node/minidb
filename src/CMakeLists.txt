# Build the core as a library
add_library(minidb_core
    schema/schema.c
    ast/ast.c
)

# The CLI executable
add_executable(minidb main.c)
target_link_libraries(minidb PRIVATE minidb_core)
