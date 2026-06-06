#include "minidb/operator.h"

#include <stdlib.h>
#include <string.h>

minidb_operator_t make_scan(minidb_table_t *table) {
    minidb_scan_state_t *state = malloc(sizeof(minidb_scan_state_t));

    state->table = table;
    state->cursor = 0;

    minidb_operator_t op;
    op.open = scan_open;
    op.close = scan_close;
    op.next = scan_next;
    op.state = state;

    return op;
}

void scan_open(minidb_operator_t *op) {
    minidb_scan_state_t *state = op->state;
    state->cursor = 0;
}

minidb_db_status scan_next(minidb_operator_t *op, minidb_structured_row_t *out_row) {
    if (op == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }

    minidb_scan_state_t *state = op->state;

    if (state->cursor >= state->table->n_rows) {
        return MINIDB_ERR_ROWS_EXHAUSTED;
    }

    size_t offset = 0;
    out_row->data = malloc(sizeof(minidb_data_t) * state->table->schema.n_cols);
    if (out_row->data == NULL) {
        return MINIDB_ERR_MEM_ALLOC_FAIL;
    }

    out_row->schema = &(state->table->schema);
    for (size_t i = 0; i < state->table->schema.n_cols; i++) {
        switch (state->table->schema.cols[i].type) {
            case ID_TYPE:  {
                memcpy(&(out_row->data[i].data.id), state->table->rows[state->cursor].bytes + offset, sizeof(uint32_t));
                offset+=sizeof(uint32_t);
                break;
            }
            case INT_TYPE: {
                memcpy(&(out_row->data[i].data.number), state->table->rows[state->cursor].bytes + offset, sizeof(int32_t));
                offset+=sizeof(int32_t);
                break;
            }
            case DECIMAL_TYPE: {
                memcpy(&(out_row->data[i].data.decimal), state->table->rows[state->cursor].bytes + offset, sizeof(double));
                offset+=sizeof(double);
                break;
            }
            case VARCHAR_TYPE: {
                memcpy(&out_row->data[i].data.varchar.length , state->table->rows[state->cursor].bytes + offset, sizeof(size_t));
                offset+=sizeof(size_t);
                out_row->data[i].data.varchar.data = malloc(out_row->data[i].data.varchar.length * sizeof(char) + 1);
                if (out_row->data[i].data.varchar.data == NULL) {
                    return MINIDB_ERR_MEM_ALLOC_FAIL;
                }
                memcpy((out_row->data[i].data.varchar.data), state->table->rows[state->cursor].bytes + offset, out_row->data[i].data.varchar.length * sizeof(char));
                offset+=out_row->data[i].data.varchar.length*sizeof(char);
                break;
            }
            case BOOL_TYPE: {
                memcpy(&out_row->data[i].data.boolean, state->table->rows[state->cursor].bytes+offset, sizeof(bool));
                offset+=sizeof(bool);
                break;
            }
        }
    }

    state->cursor++;
    return MINIDB_OK;
}

void scan_close(minidb_operator_t *op) {}
