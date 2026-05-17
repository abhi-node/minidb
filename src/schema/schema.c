#include "minidb/schema.h"
#include <stdlib.h>

#define INITIAL_TABLE_CAPACITY 32
#define TABLE_INCREASE_FACTOR 2

db_status create_table(minidb_schema_t schema, minidb_table_t* out_table) {
    out_table = malloc(sizeof(minidb_table_t));

    out_table->schema = schema;
    out_table->capacity = INITIAL_TABLE_CAPACITY;
    out_table->n_rows = 0;
    out_table->rows = malloc(INITIAL_TABLE_CAPACITY * sizeof(minidb_row_t));

    return DB_SUCCESS;
}

db_status insert_row(minidb_row_t row, minidb_table_t *table){
    if (table->n_rows == table->capacity) {
        size_t capacity = table->capacity * TABLE_INCREASE_FACTOR;
        minidb_row_t *rows = realloc(table->rows, capacity * sizeof(minidb_row_t));

        if (rows == NULL) {
            return ERR_INSERT_FAIL;
        }

        table->rows = rows;
        table->capacity = capacity;
    }

    table->rows[table->n_rows] = row;
    table->n_rows++;

    return DB_SUCCESS;
}
