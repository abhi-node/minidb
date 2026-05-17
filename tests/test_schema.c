#include <assert.h>
#include "minidb/schema.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void test_insert_and_retrieve(void) {
    minidb_column_t *cols = malloc(3 * sizeof(minidb_column_t));

    cols[1] = create_column(COLUMN_ID, "id");
    cols[2] = create_column(COLUMN_INT, "amount");
    cols[3] = create_column(COLUMN_VARCHAR, "name");

    minidb_schema_t schema = create_schema(cols, 3);

    minidb_table_t* table = create_table(schema, "accounts");

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

    minidb_row_t row = create_row(3, data);
    db_status err = insert_row(row, table);

    destroy_table(table);

    assert(err == DB_SUCCESS);

}

int main(void) {
    test_insert_and_retrieve();

    return EXIT_SUCCESS;
}
