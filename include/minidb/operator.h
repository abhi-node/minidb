#ifndef MINIDB_OPERATOR_H
#define MINIDB_OPERATOR_H

#include "minidb/schema.h"

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

void scan_open(minidb_operator_t *op);
void scan_close(minidb_operator_t *op);
minidb_db_status scan_next(minidb_operator_t *op, minidb_structured_row_t *out_row);
minidb_operator_t make_scan(minidb_table_t *table);

/*TODO: Create expression types and expression evaluate function */
/*TODO: Create filter operator functions */

#endif
