#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <math.h>
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
        double_t decimal;
        struct {
            char* data;
            uint32_t length;
        } varchar;
    } data;
} minidb_data_t;

typedef enum {
    DB_SUCCESS, // on success
    ERR_INSERT_FAIL, // insertion fail
    ERR_SELECT_FAIL // select fail
} db_status;

typedef struct {
    const char *name; // column name, used for query match - string literal as column name never changes
    minidb_column_data_t type; // column type, used for offset/size calculation
} minidb_column_t;

typedef struct {
    uint32_t n_cols; // number of cols
    minidb_column_t *cols; // raw col structs
} minidb_schema_t;

typedef struct {
    uint8_t *bytes; // raw bytes of data
    size_t size; // size represented by row (variable due to varchar)
} minidb_row_t;

typedef struct {
    minidb_schema_t schema; // table schema
    minidb_row_t *rows; // raw rows
    const char* name; // name literal
    size_t n_rows; // row number
    size_t capacity; // capacity (treat rows as a dynamic array)
} minidb_table_t;

minidb_row_t create_row(uint32_t count, minidb_data_t *data);
minidb_column_t create_column(minidb_column_data_t type, char* name);
minidb_schema_t create_schema(minidb_column_t *cols, uint32_t n_cols);

minidb_table_t* create_table(minidb_schema_t schema, char* name);
db_status destroy_table(minidb_table_t* table);

db_status insert_row(minidb_row_t row, minidb_table_t* table);
db_status print_rows(minidb_table_t* table);

#endif
