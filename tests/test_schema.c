#include <assert.h>
#include "minidb/schema.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void test_insert_and_retrieve(void) {
    minidb_column_t *cols = malloc(3 * sizeof(minidb_column_t));

    cols[1] = create_column(COLUMN_ID, "id");
    cols[2] = create_column(COLUMN_INT, "amount");
    cols[3] = create_column(COLUMN_INT, "account_count");

    minidb_schema_t schema = create_schema(cols, 3);

    minidb_table_t* table = create_table(schema, "accounts");

    uint32_t id = 1;
    uint32_t amount = 100;
    uint32_t account_count = 1;
    uint8_t *bytes = malloc(3 * sizeof(uint32_t));

    size_t offset = 0;

    memcpy(bytes+offset, &id, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    memcpy(bytes+offset, &amount, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    memcpy(bytes+ offset, &account_count, sizeof(uint32_t));

    minidb_row_t row;
    row.bytes = bytes;
    row.size = 3 * sizeof(uint32_t);

    db_status err = insert_row(row, table);

    destroy_table(table);

    assert(err == DB_SUCCESS);
}

int main(void) {
    test_insert_and_retrieve();

    return EXIT_SUCCESS;
}
