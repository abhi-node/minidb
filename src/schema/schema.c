#include "minidb/schema.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_TABLE_CAPACITY 32
#define TABLE_INCREASE_FACTOR 2

minidb_column_t create_column(minidb_column_data_t type, char *name) {
    minidb_column_t out_col;

    out_col.name = name;
    out_col.type = type;

    return out_col;
}

minidb_schema_t create_schema(minidb_column_t *cols, size_t n_cols) {
    minidb_schema_t out_schema;
    out_schema.cols = cols;
    out_schema.n_cols = n_cols;

    return out_schema;
}

minidb_table_t* create_table(minidb_schema_t schema, char* name) {
    minidb_table_t *out_table = malloc(sizeof(minidb_table_t));
    if (out_table == NULL) {
        return NULL;
    }

    out_table->schema = schema;
    out_table->capacity = INITIAL_TABLE_CAPACITY;
    out_table->n_rows = 0;
    out_table->name = name;
    out_table->rows = malloc(INITIAL_TABLE_CAPACITY * sizeof(minidb_row_t));

    if (out_table->rows == NULL) {
        destroy_table(out_table);
        return NULL;
    }

    return out_table;
}

minidb_db_status destroy_table(minidb_table_t* table) {
    if (table == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }
    for (size_t i = 0; i < table->n_rows; i++) {
        free(table->rows[i].bytes);
    }
    free(table->rows);
    free(table);

    return MINIDB_OK;
}

size_t get_data_size(uint32_t count, minidb_data_t *data) {
    size_t size = 0;
    for (uint32_t i = 0; i < count; i++) {
        switch (data[i].type) {
            case COLUMN_ID: {
                size+=sizeof(uint32_t);
                break;
            }
            case COLUMN_INT: {
                size+=sizeof(int32_t);
                break;
            }
            case COLUMN_DECIMAL: {
                size+= sizeof(double);
                break;
            }
            case COLUMN_VARCHAR: {
                size+=sizeof(uint32_t);
                size+= data[i].data.varchar.length*sizeof(char);
                break;
            }
        }
    }
    return size;
}

minidb_db_status create_row(minidb_schema_t schema, minidb_data_t *data, minidb_row_t *out_row) {
    if (data == NULL || out_row == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }
    size_t size = get_data_size(schema.n_cols, data);
    size_t offset = 0;
    out_row->bytes = malloc(size);
    if (out_row->bytes == NULL) {
        return MINIDB_ERR_MEM_ALLOC_FAIL;
    }

    for (uint32_t i = 0; i < schema.n_cols; i++) {
        if (data[i].type != schema.cols[i].type) {
            return MINIDB_ERR_SCHEMA_MISMATCH;
        }
        switch(schema.cols[i].type) {
            case COLUMN_ID: {
                memcpy(out_row->bytes+offset, &(data[i].data.id), sizeof(uint32_t));
                offset+=sizeof(uint32_t);
                break;
            }
            case COLUMN_INT: {
                memcpy(out_row->bytes+offset, &(data[i].data.integer), sizeof(int32_t));
                offset+=sizeof(int32_t);
                break;
            }
            case COLUMN_DECIMAL: {
                memcpy(out_row->bytes+offset, &(data[i].data.decimal), sizeof(double));
                offset+=sizeof(double);
                break;
            }
            case COLUMN_VARCHAR: {
                if (data[i].data.varchar.length > 256) {
                    return MINIDB_ERR_SCHEMA_MISMATCH;
                }
                memcpy(out_row->bytes+offset, &(data[i].data.varchar.length), sizeof(uint32_t));
                offset+=sizeof(uint32_t);
                memcpy(out_row->bytes+offset, (data[i].data.varchar.data), data[i].data.varchar.length*sizeof(char));
                offset+=data[i].data.varchar.length*sizeof(char);
                break;
            }
        }
    }

    return MINIDB_OK;
}

minidb_db_status print_row(uint32_t row_index, minidb_table_t *table) {
    size_t offset = 0;
    for (uint32_t i = 0; i < table->schema.n_cols; i++) {
        switch (table->schema.cols[i].type) {
            case COLUMN_ID:  {
                uint32_t id;
                memcpy(&id, table->rows[row_index].bytes + offset, sizeof(uint32_t));
                printf("%d\t", id);
                offset+=sizeof(uint32_t);
                break;
            }
            case COLUMN_INT: {
                int32_t integer;
                memcpy(&integer, table->rows[row_index].bytes + offset, sizeof(int32_t));
                printf("%d\t", integer);
                offset+=sizeof(int32_t);
                break;
            }
            case COLUMN_DECIMAL: {
                double decimal;
                memcpy(&decimal, table->rows[row_index].bytes + offset, sizeof(double));
                printf("%0.2f\t", decimal);
                offset+=sizeof(double);
                break;
            }
            case COLUMN_VARCHAR: {
                uint32_t length;
                memcpy(&length, table->rows[row_index].bytes + offset, sizeof(uint32_t));
                offset+=sizeof(uint32_t);

                char* data = malloc(length * sizeof(char));
                if (data == NULL) {
                    return MINIDB_ERR_MEM_ALLOC_FAIL;
                }
                memcpy(data, table->rows[row_index].bytes + offset, length * sizeof(char));
                printf("%.*s\t", length, data);
                offset+=length*sizeof(char);
                free(data);
                break;
            }
        }
    }

    printf("\n");

    return MINIDB_OK;
}

minidb_db_status print_rows(minidb_table_t *table) {
    for (uint32_t i = 0; i < table->n_rows; i++) {
        minidb_db_status print_status = print_row(i, table);
        if (print_status != MINIDB_OK) {
            return print_status;
        }
    }

    return MINIDB_OK;
}

minidb_db_status insert_row(minidb_data_t *data, size_t n_data, minidb_table_t *table){
    if (data == NULL || table == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }

    if (n_data != table->schema.n_cols) {
        return MINIDB_ERR_SCHEMA_MISMATCH;
    }

    minidb_row_t row;
    minidb_db_status create_status = create_row(table->schema, data, &row);
    if (create_status != MINIDB_OK) {
        return create_status;
    }

    if (table->n_rows == table->capacity) {
        minidb_row_t *rows = realloc(table->rows, TABLE_INCREASE_FACTOR*sizeof(minidb_row_t)* table->capacity);
        if (rows == NULL) {
            return MINIDB_ERR_MEM_ALLOC_FAIL;
        }

        table->rows = rows;
        table->capacity = TABLE_INCREASE_FACTOR*sizeof(minidb_row_t)* table->capacity;
    }

    table->rows[table->n_rows] = row;
    table->n_rows++;

    return MINIDB_OK;
}
