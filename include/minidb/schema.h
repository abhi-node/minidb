#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

// Represents column data types
typedef enum {
    COLUMN_ID = 0, // ID - required 4 byte integer id, configured by system
    COLUMN_INT, // INT - 4 byte integer
    COLUMN_DECIMAL, // DECIMAL - 8 byte float
    COLUMN_VARCHAR // VARCHAR - 256 byte string of variable length
} minidb_column_data_t;

typedef struct {
    minidb_column_data_t type;
    union {
        uint32_t id;
        int32_t integer;
        double decimal;
        struct {
            char* data;
            size_t length;
        } varchar;
    } data;
} minidb_data_t;

typedef enum {
    MINIDB_OK, // on success
    MINIDB_ERR_INSERT_FAIL, // insertion fail
    MINIDB_ERR_SELECT_FAIL,
    MINIDB_ERR_SCHEMA_MISMATCH,
    MINIDB_ERR_MEM_ALLOC_FAIL,
    MINIDB_ERR_INVALID_CALL,
    MINIDB_ERR_INVALID_SCHEMA
} minidb_db_status;

typedef struct {
    const char *name; // column name, used for query match - string literal as column name never changes
    minidb_column_data_t type; // column type, used for offset/size calculation
} minidb_column_t;

typedef struct {
    size_t n_cols; // number of cols
    minidb_column_t *cols; // raw col structs
} minidb_schema_t;

typedef struct {
    char *bytes; // raw bytes of data
    size_t size; // size represented by row (variable due to varchar)
} minidb_row_t;

typedef struct {
    minidb_data_t *data;
    size_t n_cols;
} minidb_structured_row_t;

typedef struct {
    minidb_schema_t schema; // table schema
    minidb_row_t *rows; // raw rows
    const char* name; // name literal
    size_t n_rows; // row number
    size_t capacity; // capacity (treat rows as a dynamic array)
} minidb_table_t;


minidb_db_status create_table(minidb_column_data_t* types, char** names, size_t n_cols, char* name, minidb_table_t *out_table);
minidb_db_status destroy_table(minidb_table_t *table);

minidb_db_status insert_row(minidb_data_t *data, size_t n_data, minidb_table_t *table);


minidb_db_status select_all_rows(minidb_table_t *table, minidb_structured_row_t **rows, size_t *out_size);
minidb_db_status select_rows(minidb_table_t *table);
minidb_db_status select_where_rows(minidb_table_t *table);

#endif
