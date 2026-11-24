#ifndef MINIDB_OPERATOR_H
#define MINIDB_OPERATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "minidb/arena.h"
#include "minidb/ast.h"
#include "minidb/catalog.h"
#include "minidb/expression.h"
#include "minidb/pager.h"
#include "minidb/status.h"
#include "minidb/value.h"

/*
 * Volcano-model query operators.
 *
 * A query is a tree of operators, each of which is an iterator over rows.
 * The executor pulls rows from the root one at a time; each operator pulls
 * from its children as needed, so no intermediate result is materialized
 * (except by Sort, which has to see every row first).
 *
 *   SELECT u.name FROM users u JOIN orders o ON o.user_id = u.id
 *   WHERE o.total > 10 LIMIT 5
 *
 *       Limit(5)
 *         Project(u.name)
 *           Filter(o.total > 10)
 *             Join(o.user_id = u.id)
 *               Scan(users)   Scan(orders)
 *
 * The protocol is open() -> next()* -> close(). An operator may be opened
 * again after it is closed, which restarts it from the first row; a join
 * uses this to rescan its right side.
 *
 * Ownership:
 *   - Operators and their row buffers live in the statement's arena.
 *   - The row returned by next() is borrowed. It, and any TEXT it points to,
 *     is valid only until the next call to next() or close() on the same
 *     operator.
 *   - close() releases everything that is not arena memory (pinned pages,
 *     sort buffers) and closes the operator's children.
 */

typedef struct minidb_operator minidb_operator_t;

typedef struct {
    const char *name;
    minidb_status_t (*open)(minidb_operator_t *self);
    /* MINIDB_OK with a row, MINIDB_DONE when exhausted, or an error. */
    minidb_status_t (*next)(minidb_operator_t *self, const minidb_value_t **out_row);
    void (*close)(minidb_operator_t *self);
} minidb_operator_vtable_t;

/* Common header; each operator type embeds it as its first member. */
struct minidb_operator {
    const minidb_operator_vtable_t *vtable;
    minidb_row_shape_t shape;  /* columns of the rows this operator produces */
};

static inline minidb_status_t minidb_operator_open(minidb_operator_t *op) {
    return op->vtable->open(op);
}

static inline minidb_status_t minidb_operator_next(minidb_operator_t *op,
                                                   const minidb_value_t **out_row) {
    return op->vtable->next(op, out_row);
}

static inline void minidb_operator_close(minidb_operator_t *op) {
    op->vtable->close(op);
}

/* ---- Scan: rows of one table, in primary-key order ----------------------- */

/* `alias` is the name the query uses for the table's columns. */
minidb_status_t minidb_scan_create(minidb_arena_t *arena, minidb_pager_t *pager,
                                   const minidb_table_t *table, const char *alias,
                                   minidb_operator_t **out_scan);

/* Restricts the scan to keys in [first_key, last_key]. Call before open(). */
void minidb_scan_set_key_range(minidb_operator_t *scan, int64_t first_key, int64_t last_key);

/* B+-tree key of the row most recently returned by next(). */
int64_t minidb_scan_current_key(const minidb_operator_t *scan);

/* ---- Filter: rows of the child for which the predicate is true ----------- */

/* `predicate` must be bound to the child's shape and of type BOOL. */
minidb_status_t minidb_filter_create(minidb_arena_t *arena, minidb_operator_t *child,
                                     const minidb_expr_t *predicate,
                                     minidb_operator_t **out_filter);

/* ---- Project: one computed column per expression ------------------------- */

/* `exprs` are bound to the child's shape; `fields` describes the output. */
minidb_status_t minidb_project_create(minidb_arena_t *arena, minidb_operator_t *child,
                                      minidb_expr_t **exprs, minidb_field_t *fields,
                                      uint32_t column_count, minidb_operator_t **out_project);

/* ---- Joins: each output row is a left row followed by a right row -------- */

/*
 * Nested-loop join: for every left row, rescan `right` and emit the pairs
 * that satisfy `predicate` (bound to the combined shape; NULL = all pairs).
 */
minidb_status_t minidb_nested_loop_join_create(minidb_arena_t *arena, minidb_operator_t *left,
                                               minidb_operator_t *right,
                                               const minidb_expr_t *predicate,
                                               minidb_operator_t **out_join);

/*
 * Index join: for every left row, evaluate `key_expr` (an INT expression
 * bound to the left shape) and look that key up in `right_table`'s B+-tree.
 * O(log n) per left row instead of a full rescan.
 */
minidb_status_t minidb_index_join_create(minidb_arena_t *arena, minidb_operator_t *left,
                                         minidb_pager_t *pager,
                                         const minidb_table_t *right_table,
                                         const char *right_alias, const minidb_expr_t *key_expr,
                                         minidb_operator_t **out_join);

/* ---- Sort: all rows of the child, ordered -------------------------------- */

/* `terms` are bound to the child's shape. Earlier terms take precedence. */
minidb_status_t minidb_sort_create(minidb_arena_t *arena, minidb_operator_t *child,
                                   const minidb_order_term_t *terms, uint32_t term_count,
                                   minidb_operator_t **out_sort);

/* ---- Limit: the first `row_limit` rows of the child ---------------------- */

minidb_status_t minidb_limit_create(minidb_arena_t *arena, minidb_operator_t *child,
                                    int64_t row_limit, minidb_operator_t **out_limit);

#endif
