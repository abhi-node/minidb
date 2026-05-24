#include "minidb/schema.h"
#include <stdlib.h>


int main(void) {
    minidb_column_data_t *types = malloc(sizeof(minidb_column_data_t)*3);
    char **names = malloc(sizeof(char*)*3);

    types[0] = COLUMN_ID;
    types[1] = COLUMN_INT;
    types[2] = COLUMN_VARCHAR;

    names[0] = "id";
    names[1] = "amount";
    names[2] = "name";


    minidb_table_t *table = malloc(sizeof(minidb_table_t));
    create_table(types, names, 3, "accounts", table);

    minidb_data_t data[3];

    data[0] = (minidb_data_t) {
        .type = COLUMN_ID,
        .data = {
            .id = 0
        }
    };
    data[1] = (minidb_data_t) {
        .type = COLUMN_INT,
        .data = {
            .integer = 100
        }
    };
    data[2] = (minidb_data_t) {
        .type = COLUMN_VARCHAR,
        .data = {
            .varchar = {
                .length = 8,
                .data = "Abhijith"
            }
        }
    };

    minidb_db_status insert_status = insert_row(data, 3, table);
    if (insert_status != MINIDB_OK) {
        return insert_status;
    }

    minidb_db_status print_status = print_rows(table);
    if (print_status != MINIDB_OK) {
        return print_status;
    }

    destroy_table(table);

}
