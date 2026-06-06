#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VARCHAR_MAX_SIZE 256

typedef enum {
    BOOL_TYPE = 0,
    INT_TYPE,
    DECIMAL_TYPE,
    VARCHAR_TYPE,
    ID_TYPE
} minidb_return_t;

typedef struct {
    minidb_return_t type;
    union {
        uint32_t id;
        int32_t number;
        double decimal;
        struct {
            char* data;
            size_t length;
        } varchar;
        bool boolean;
    } data;
} minidb_data_t;

typedef enum {
    MINIDB_OK,
    MINIDB_ERR_INSERT_FAIL,
    MINIDB_ERR_SELECT_FAIL,
    MINIDB_ERR_SCHEMA_MISMATCH,
    MINIDB_ERR_MEM_ALLOC_FAIL,
    MINIDB_ERR_INVALID_CALL,
    MINIDB_ERR_INVALID_SCHEMA,
    MINIDB_ERR_ROWS_EXHAUSTED
} minidb_db_status;

typedef struct {
    const char *name;
    minidb_return_t type;
} minidb_column_t;

typedef struct {
    size_t n_cols;
    minidb_column_t *cols;
} minidb_schema_t;

typedef struct {
    char *bytes;
    size_t size;
} minidb_row_t;

typedef struct {
    minidb_data_t *data;
    size_t n_cols;
    minidb_column_t *cols;
} minidb_structured_row_t;

typedef struct {
    minidb_schema_t schema;
    minidb_row_t *rows;
    const char* name;
    size_t n_rows;
    size_t capacity;
} minidb_table_t;

minidb_db_status create_table(minidb_return_t* types, char** names, size_t n_cols, char* name, minidb_table_t *out_table);
minidb_db_status destroy_table(minidb_table_t *table);

minidb_db_status insert_row(minidb_data_t *data, size_t n_data, minidb_table_t *table);

#endif
