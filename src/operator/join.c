#include <string.h>

#include "minidb/btree.h"
#include "minidb/operator.h"
#include "minidb/row.h"

/*
 * Both joins emit a combined row: the left row's values followed by the
 * right row's. The combined buffer belongs to the join; TEXT values in it
 * still borrow from the children's current rows.
 */

/* Allocates the fields of a combined shape: left's, then `right_count` more. */
static minidb_field_t *combined_fields(minidb_arena_t *arena, const minidb_row_shape_t *left,
                                       uint32_t right_count) {
    minidb_field_t *fields =
        minidb_arena_alloc(arena, (left->field_count + right_count) * sizeof(*fields));
    if (fields != NULL) {
        memcpy(fields, left->fields, left->field_count * sizeof(*fields));
    }
    return fields;
}

/* ---- Nested-loop join ---------------------------------------------------- */

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *left;
    minidb_operator_t *right;
    const minidb_expr_t *predicate;  /* NULL joins every pair */
    bool has_left_row;               /* a left row is loaded and `right` is mid-scan */
    minidb_value_t *values;          /* combined row */
} nested_loop_join_t;

static minidb_status_t nested_loop_join_open(minidb_operator_t *self) {
    nested_loop_join_t *join = (nested_loop_join_t *)self;
    join->has_left_row = false;
    return minidb_operator_open(join->left);
}

static minidb_status_t nested_loop_join_next(minidb_operator_t *self,
                                             const minidb_value_t **out_row) {
    nested_loop_join_t *join = (nested_loop_join_t *)self;
    uint32_t left_count = join->left->shape.field_count;
    uint32_t right_count = join->right->shape.field_count;

    for (;;) {
        if (!join->has_left_row) {
            /* Advance the outer side and restart the inner scan. */
            const minidb_value_t *left_row;
            minidb_status_t status = minidb_operator_next(join->left, &left_row);
            if (status != MINIDB_OK) {
                return status;
            }
            memcpy(join->values, left_row, left_count * sizeof(*left_row));

            minidb_operator_close(join->right);
            status = minidb_operator_open(join->right);
            if (status != MINIDB_OK) {
                return status;
            }
            join->has_left_row = true;
        }

        const minidb_value_t *right_row;
        minidb_status_t status = minidb_operator_next(join->right, &right_row);
        if (status == MINIDB_DONE) {
            join->has_left_row = false;
            continue;
        }
        if (status != MINIDB_OK) {
            return status;
        }
        memcpy(join->values + left_count, right_row, right_count * sizeof(*right_row));

        minidb_value_t is_match = minidb_value_bool(true);
        if (join->predicate != NULL) {
            status = minidb_expr_evaluate(join->predicate, join->values, &is_match);
            if (status != MINIDB_OK) {
                return status;
            }
        }
        if (is_match.as.boolean) {
            *out_row = join->values;
            return MINIDB_OK;
        }
    }
}

static void nested_loop_join_close(minidb_operator_t *self) {
    nested_loop_join_t *join = (nested_loop_join_t *)self;
    minidb_operator_close(join->right);
    minidb_operator_close(join->left);
}

static const minidb_operator_vtable_t NESTED_LOOP_JOIN_VTABLE = {
    "NestedLoopJoin", nested_loop_join_open, nested_loop_join_next, nested_loop_join_close};

minidb_status_t minidb_nested_loop_join_create(minidb_arena_t *arena, minidb_operator_t *left,
                                               minidb_operator_t *right,
                                               const minidb_expr_t *predicate,
                                               minidb_operator_t **out_join) {
    uint32_t left_count = left->shape.field_count;
    uint32_t right_count = right->shape.field_count;

    nested_loop_join_t *join = minidb_arena_alloc_zeroed(arena, sizeof(*join));
    minidb_field_t *fields = combined_fields(arena, &left->shape, right_count);
    minidb_value_t *values =
        minidb_arena_alloc(arena, (left_count + right_count) * sizeof(*values));
    if (join == NULL || fields == NULL || values == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    memcpy(fields + left_count, right->shape.fields, right_count * sizeof(*fields));

    join->base.vtable = &NESTED_LOOP_JOIN_VTABLE;
    join->base.shape.fields = fields;
    join->base.shape.field_count = left_count + right_count;
    join->left = left;
    join->right = right;
    join->predicate = predicate;
    join->values = values;
    *out_join = &join->base;
    return MINIDB_OK;
}

/* ---- Index join ---------------------------------------------------------- */

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *left;
    const minidb_table_t *right_table;
    const minidb_expr_t *key_expr;  /* computes the right table's key from a left row */
    minidb_cursor_t cursor;         /* keeps the matched right row's leaf pinned */
    minidb_value_t *values;         /* combined row */
} index_join_t;

static minidb_status_t index_join_open(minidb_operator_t *self) {
    index_join_t *join = (index_join_t *)self;
    minidb_cursor_close(&join->cursor);
    return minidb_operator_open(join->left);
}

static minidb_status_t index_join_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    index_join_t *join = (index_join_t *)self;
    uint32_t left_count = join->left->shape.field_count;

    for (;;) {
        const minidb_value_t *left_row;
        minidb_status_t status = minidb_operator_next(join->left, &left_row);
        if (status != MINIDB_OK) {
            return status;
        }

        minidb_value_t key;
        status = minidb_expr_evaluate(join->key_expr, left_row, &key);
        if (status != MINIDB_OK) {
            return status;
        }

        /* A primary key is unique, so each left row matches at most one right row. */
        status = minidb_cursor_seek(&join->cursor, join->right_table->root_page, key.as.integer);
        if (status != MINIDB_OK) {
            return status;
        }
        if (!minidb_cursor_is_valid(&join->cursor) ||
            minidb_cursor_key(&join->cursor) != key.as.integer) {
            continue;
        }

        uint32_t row_length;
        const uint8_t *row_bytes = minidb_cursor_payload(&join->cursor, &row_length);
        status = minidb_row_decode(&join->right_table->schema, row_bytes, row_length,
                                   join->values + left_count);
        if (status != MINIDB_OK) {
            return status;
        }
        memcpy(join->values, left_row, left_count * sizeof(*left_row));
        *out_row = join->values;
        return MINIDB_OK;
    }
}

static void index_join_close(minidb_operator_t *self) {
    index_join_t *join = (index_join_t *)self;
    minidb_cursor_close(&join->cursor);
    minidb_operator_close(join->left);
}

static const minidb_operator_vtable_t INDEX_JOIN_VTABLE = {"IndexJoin", index_join_open,
                                                           index_join_next, index_join_close};

minidb_status_t minidb_index_join_create(minidb_arena_t *arena, minidb_operator_t *left,
                                         minidb_pager_t *pager,
                                         const minidb_table_t *right_table,
                                         const char *right_alias, const minidb_expr_t *key_expr,
                                         minidb_operator_t **out_join) {
    uint32_t left_count = left->shape.field_count;
    uint32_t right_count = right_table->schema.column_count;

    index_join_t *join = minidb_arena_alloc_zeroed(arena, sizeof(*join));
    minidb_field_t *fields = combined_fields(arena, &left->shape, right_count);
    minidb_value_t *values =
        minidb_arena_alloc(arena, (left_count + right_count) * sizeof(*values));
    if (join == NULL || fields == NULL || values == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    for (uint32_t index = 0; index < right_count; index++) {
        fields[left_count + index].table = right_alias;
        fields[left_count + index].name = right_table->schema.columns[index].name;
        fields[left_count + index].type = right_table->schema.columns[index].type;
    }

    join->base.vtable = &INDEX_JOIN_VTABLE;
    join->base.shape.fields = fields;
    join->base.shape.field_count = left_count + right_count;
    join->left = left;
    join->right_table = right_table;
    join->key_expr = key_expr;
    join->values = values;
    minidb_cursor_init(&join->cursor, pager);
    *out_join = &join->base;
    return MINIDB_OK;
}
