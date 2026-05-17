#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <stddef.h>

typedef enum {
    COLUMN_ID = 0,
    COLUMN_INT,
    COLUMN_DECIMAL
} minidb_var_t;

typedef enum {
    DB_SUCCESS,
    ERR_INSERT_FAIL,
    ERR_SELECT_FAIL
} db_status;

typedef struct {
    const char *name;
    minidb_var_t type;
} minidb_column_t;

typedef struct {
    size_t size;
    size_t offset;
    minidb_column_t *cols;
} minidb_schema_t;

typedef struct {
    char *bytes;
} minidb_row_t;

typedef struct {
    minidb_schema_t schema;
    minidb_row_t *rows;
    size_t n_rows;
    size_t capacity;
} minidb_table_t;

db_status create_table(minidb_schema_t schema, minidb_table_t* out_table);
db_status insert_row(minidb_row_t row, minidb_table_t* table);

#endif
