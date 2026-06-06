#include <assert.h>
#include "minidb/schema.h"
#include "minidb/operator.h"
#include <stdlib.h>
#include <string.h>

void test_insert_and_retrieve(void) {
    minidb_return_t *types = malloc(sizeof(minidb_return_t)*3);
    char **names = malloc(sizeof(char*)*3);

    types[0] = ID_TYPE;
    types[1] = INT_TYPE;
    types[2] = VARCHAR_TYPE;

    names[0] = "id";
    names[1] = "amount";
    names[2] = "name";


    minidb_table_t *table = malloc(sizeof(minidb_table_t));
    minidb_db_status table_status = create_table(types, names, 3, "accounts", table);
    assert(table_status == MINIDB_OK);

    minidb_data_t data[3];

    data[0] = (minidb_data_t) {
        .type = ID_TYPE,
        .data = {
            .id = 0
        }
    };
    data[1] = (minidb_data_t) {
        .type = INT_TYPE,
        .data = {
            .number = 100
        }
    };
    data[2] = (minidb_data_t) {
        .type = VARCHAR_TYPE,
        .data = {
            .varchar = {
                .length = 8,
                .data = "Abhijith"
            }
        }
    };

    minidb_db_status insert_status = insert_row(data, 3, table);
    assert(insert_status == MINIDB_OK);

    minidb_structured_row_t out_data;

    minidb_operator_t scan_op = make_scan(table);
    scan_open(&scan_op);
    scan_next(&scan_op, &out_data);
    scan_close(&scan_op);

    assert(strcmp("Abhijith", out_data.data[2].data.varchar.data) ==0);

    destroy_table(table);

}

int main(void) {
    test_insert_and_retrieve();

    return EXIT_SUCCESS;
}


/*
 *
 * TODO: write tests, specifically unit tests - integration is chill
 */
