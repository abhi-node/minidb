add_executable(test_table test_table.c)
target_link_libraries(test_table PRIVATE minidb_core)
add_test(NAME table COMMAND test_table)
