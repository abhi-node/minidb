#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#define VARCHAR_MAX_SIZE 256

typedef enum {
    COLUMN_ID = 0,
    COLUMN_INT,
    COLUMN_DECIMAL,
    COLUMN_VARCHAR
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
    minidb_column_data_t type;
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
    minidb_schema_t *schema;
} minidb_structured_row_t;

typedef struct {
    minidb_schema_t schema;
    minidb_row_t *rows;
    const char* name;
    size_t n_rows;
    size_t capacity;
} minidb_table_t;



typedef struct minidb_operator_t minidb_operator_t;

struct minidb_operator_t {
    void (*open)(minidb_operator_t *op);
    void (*close)(minidb_operator_t *op);
    minidb_db_status (*next)(minidb_operator_t *op, minidb_structured_row_t *out_row);

    void* state;
};

typedef struct {
    minidb_table_t *table;
    size_t cursor;
} minidb_scan_state_t;


minidb_db_status create_table(minidb_column_data_t* types, char** names, size_t n_cols, char* name, minidb_table_t *out_table);
minidb_db_status destroy_table(minidb_table_t *table);

minidb_db_status insert_row(minidb_data_t *data, size_t n_data, minidb_table_t *table);

void scan_open(minidb_operator_t *op);
void scan_close(minidb_operator_t *op);
minidb_db_status scan_next(minidb_operator_t *op, minidb_structured_row_t *out_row);
minidb_operator_t make_scan(minidb_table_t *table);


/*TODO: Create expression types and expression evaluate function */
/*TODO: Create filter operator functions */

#endif
