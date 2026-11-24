# One executable per module: minidb_add_test(<module>) builds tests/test_<module>.c.
function(minidb_add_test module)
    add_executable(test_${module} test_${module}.c)
    target_link_libraries(test_${module} PRIVATE minidb_core)
    add_test(NAME ${module} COMMAND test_${module})
endfunction()

minidb_add_test(row)
minidb_add_test(pager)
minidb_add_test(btree)
minidb_add_test(wal)
minidb_add_test(storage)
