#ifndef MINIDB_AST_H
#define MINIDB_AST_H

#include <stdbool.h>
#include <stdint.h>

#include "minidb/schema.h"
#include "minidb/value.h"

/*
 * Abstract syntax tree produced by the parser.
 *
 * The parser only records what was written. Names are resolved and types
 * are checked later, by the binder (see expression.h), which fills in the
 * fields marked "set by the binder".
 *
 * Ownership: every node, list and string lives in the arena the statement
 * was parsed into, and is freed with it. Identifiers are lower-cased,
 * NUL-terminated copies.
 */

/* ---- Expressions --------------------------------------------------------- */

typedef enum {
    MINIDB_EXPR_LITERAL,
    MINIDB_EXPR_COLUMN,
    MINIDB_EXPR_UNARY,
    MINIDB_EXPR_BINARY
} minidb_expr_kind_t;

typedef enum {
    MINIDB_OP_NEGATE,  /* -x */
    MINIDB_OP_NOT
} minidb_unary_op_t;

typedef enum {
    MINIDB_OP_ADD,
    MINIDB_OP_SUBTRACT,
    MINIDB_OP_MULTIPLY,
    MINIDB_OP_DIVIDE,
    MINIDB_OP_EQUAL,
    MINIDB_OP_NOT_EQUAL,
    MINIDB_OP_LESS,
    MINIDB_OP_LESS_EQUAL,
    MINIDB_OP_GREATER,
    MINIDB_OP_GREATER_EQUAL,
    MINIDB_OP_AND,
    MINIDB_OP_OR,
    MINIDB_OP_LIKE
} minidb_binary_op_t;

typedef struct minidb_expr minidb_expr_t;

struct minidb_expr {
    minidb_expr_kind_t kind;
    minidb_type_t type;  /* result type; set by the binder */
    union {
        minidb_value_t literal;
        struct {
            const char *table;  /* qualifier as written, or NULL */
            const char *name;
            uint32_t index;     /* position in the input row; set by the binder */
        } column;
        struct {
            minidb_unary_op_t op;
            minidb_expr_t *operand;
        } unary;
        struct {
            minidb_binary_op_t op;
            minidb_expr_t *left;
            minidb_expr_t *right;
        } binary;
    } as;
};

/* ---- Statements ---------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *alias;  /* the name itself when no alias was written */
} minidb_table_ref_t;

typedef struct {
    minidb_table_ref_t table;
    minidb_expr_t *condition;  /* the ON expression */
} minidb_join_t;

typedef struct {
    minidb_expr_t *expr;
    const char *alias;  /* NULL when the item has no AS name */
} minidb_select_item_t;

typedef struct {
    minidb_expr_t *expr;
    bool is_descending;
} minidb_order_term_t;

typedef struct {
    bool selects_all_columns;  /* SELECT * */
    minidb_select_item_t *items;
    uint32_t item_count;
    minidb_table_ref_t from;
    minidb_join_t *joins;
    uint32_t join_count;
    minidb_expr_t *where;  /* NULL when absent */
    minidb_order_term_t *order_terms;
    uint32_t order_term_count;
    bool has_limit;
    int64_t limit;
} minidb_select_statement_t;

typedef struct {
    const char *table_name;
    const char **column_names;  /* explicit column list, or NULL for schema order */
    uint32_t column_name_count;
    minidb_expr_t **values;     /* row_count * values_per_row, row after row */
    uint32_t row_count;
    uint32_t values_per_row;
} minidb_insert_statement_t;

typedef struct {
    const char *column_name;
    minidb_expr_t *value;
} minidb_assignment_t;

typedef struct {
    const char *table_name;
    minidb_assignment_t *assignments;
    uint32_t assignment_count;
    minidb_expr_t *where;  /* NULL when absent */
} minidb_update_statement_t;

typedef struct {
    const char *table_name;
    minidb_expr_t *where;  /* NULL when absent */
} minidb_delete_statement_t;

typedef enum {
    MINIDB_STATEMENT_SELECT,
    MINIDB_STATEMENT_INSERT,
    MINIDB_STATEMENT_UPDATE,
    MINIDB_STATEMENT_DELETE,
    MINIDB_STATEMENT_CREATE_TABLE,
    MINIDB_STATEMENT_DROP_TABLE
} minidb_statement_kind_t;

typedef struct {
    minidb_statement_kind_t kind;
    union {
        minidb_select_statement_t select;
        minidb_insert_statement_t insert;
        minidb_update_statement_t update;
        minidb_delete_statement_t delete_rows;
        minidb_schema_t *create_table;  /* the new table's schema */
        const char *drop_table;         /* the table's name */
    } as;
} minidb_statement_t;

#endif
