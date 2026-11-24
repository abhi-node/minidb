# The engine is one static library; the CLI, tests and benchmarks link against it.
add_library(minidb_core
    common/status.c
    common/arena.c
    common/checksum.c
    common/failpoint.c
    value/value.c
    schema/schema.c
    row/row.c
    pager/pager.c
    btree/btree.c
    wal/wal.c
    catalog/catalog.c
    storage/storage.c
    storage/recovery.c
    lexer/lexer.c
    parser/parser.c
)

add_executable(minidb main.c)
target_link_libraries(minidb PRIVATE minidb_core)
