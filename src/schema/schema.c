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

minidb_schema_t create_schema(minidb_column_t *cols, uint32_t n_cols) {
    minidb_schema_t out_schema;
    out_schema.cols = cols;
    out_schema.n_cols = n_cols;

    return out_schema;
}

minidb_table_t* create_table(minidb_schema_t schema, char* name) {
    minidb_table_t *out_table = malloc(sizeof(minidb_table_t));

    out_table->schema = schema;
    out_table->capacity = INITIAL_TABLE_CAPACITY;
    out_table->n_rows = 0;
    out_table->name = name;
    out_table->rows = malloc(INITIAL_TABLE_CAPACITY * sizeof(minidb_row_t));

    return out_table;
}

db_status destroy_table(minidb_table_t* table) {
    free(table->rows);
    free(table);

    return DB_SUCCESS;
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
                size+= sizeof(double_t);
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

minidb_row_t create_row(uint32_t count, minidb_data_t *data) {
    minidb_row_t out_row;
    size_t size = get_data_size(count, data);
    size_t offset = 0;
    out_row.bytes = malloc(size);

    for (uint32_t i = 0; i < count; i++) {
        switch(data[i].type) {
            case COLUMN_ID: {
                memcpy(out_row.bytes+offset, &(data[i].data.id), sizeof(uint32_t));
                offset+=sizeof(uint32_t);
                break;
            }
            case COLUMN_INT: {
                memcpy(out_row.bytes+offset, &(data[i].data.integer), sizeof(int32_t));
                offset+=sizeof(int32_t);
                break;
            }
            case COLUMN_DECIMAL: {
                memcpy(out_row.bytes+offset, &(data[i].data.decimal), sizeof(double_t));
                offset+=sizeof(double_t);
                break;
            }
            case COLUMN_VARCHAR: {
                memcpy(out_row.bytes+offset, &(data[i].data.varchar.length), sizeof(uint32_t));
                offset+=sizeof(uint32_t);
                memcpy(out_row.bytes+offset, (data[i].data.varchar.data), data[i].data.varchar.length*sizeof(char));
                offset+=data[i].data.varchar.length*sizeof(char);
                break;
            }
        }
    }

    return out_row;
}

db_status print_row(uint32_t row_index, minidb_table_t *table) {
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
                double_t decimal;
                memcpy(&decimal, table->rows[row_index].bytes + offset, sizeof(double_t));
                printf("%0.2f\t", decimal);
                offset+=sizeof(double_t);
                break;
            }
            case COLUMN_VARCHAR: {
                uint32_t length;
                memcpy(&length, table->rows[row_index].bytes + offset, sizeof(uint32_t));
                offset+=sizeof(uint32_t);

                char* data = malloc(length * sizeof(char));
                memcpy(data, table->rows[row_index].bytes + offset, length * sizeof(char));
                printf("%.*s\t", length, data);
                offset+=length*sizeof(char);
                break;
            }
        }
    }

    printf("\n");

    return DB_SUCCESS;
}

db_status print_rows(minidb_table_t *table) {
    for (uint32_t i = 0; i < table->n_rows; i++) {
        print_row(i, table);
    }

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
