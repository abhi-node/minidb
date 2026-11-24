#ifndef MINIDB_EXPRESSION_H
#define MINIDB_EXPRESSION_H

#include <stdint.h>

#include "minidb/ast.h"
#include "minidb/status.h"
#include "minidb/value.h"

/*
 * Binding and evaluating expressions.
 *
 * An expression is evaluated against one row. Before that it is bound once
 * to the shape of those rows, which is where all checking happens:
 *
 *   bind      resolves each column name to a position in the row, infers the
 *             result type of every node, and rejects ill-typed expressions
 *             (e.g. name > 5, or a WHERE that is not a boolean)
 *   evaluate  walks the bound tree for one row; it cannot hit a type error
 *
 *   WHERE age > 18 AND name = 'Ada'        row: (id, name, age)
 *
 *            AND : BOOL
 *           /          \
 *       > : BOOL      = : BOOL
 *       /     \       /      \
 *   age#2:INT  18  name#1:TEXT  'Ada'
 */

/* One column of an operator's output: where it came from and its type. */
typedef struct {
    const char *table;  /* table alias the column belongs to, or NULL for a computed column */
    const char *name;
    minidb_type_t type;
} minidb_field_t;

/* The columns of the rows an operator produces. */
typedef struct {
    minidb_field_t *fields;
    uint32_t field_count;
} minidb_row_shape_t;

/*
 * Resolves and type-checks `expr` against `shape`. Pass a NULL shape for an
 * expression that may not reference columns (e.g. INSERT values).
 */
minidb_status_t minidb_expr_bind(minidb_expr_t *expr, const minidb_row_shape_t *shape,
                                 minidb_error_t *error);

/* Binds `expr` and additionally requires its result to be a BOOL. */
minidb_status_t minidb_expr_bind_predicate(minidb_expr_t *expr, const minidb_row_shape_t *shape,
                                           const char *clause, minidb_error_t *error);

/*
 * Evaluates a bound expression for one row. TEXT results borrow from the
 * row or from the expression's literals. The only runtime failure is
 * MINIDB_ERR_DIVISION_BY_ZERO.
 */
minidb_status_t minidb_expr_evaluate(const minidb_expr_t *expr, const minidb_value_t *row,
                                     minidb_value_t *out_value);

#endif
