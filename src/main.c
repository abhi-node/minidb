#include "minidb/expression.h"
#include "minidb/schema.h"
#include "minidb/operator.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>


int main(void) {
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
            .id = 200
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

    minidb_expr_t l_expr = make_int(500);
    minidb_expr_t r_expr = make_column(6, "amount");
    minidb_expr_t expr = make_expr(LESS, &l_expr, &r_expr);

    minidb_operator_t scan_op = make_scan(table);
    minidb_operator_t filter_op = make_filter(&expr, &scan_op);

    filter_op.open(&filter_op);
    filter_op.next(&filter_op, &out_data);
    filter_op.close(&filter_op);

    printf("%d", out_data.data[0].data.id);

    destroy_table(table);

    return 0;
}
