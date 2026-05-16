add_executable(test_schema test_schema.c)
target_link_libraries(test_schema PRIVATE minidb_core)
add_test(NAME schema COMMAND test_schema)
