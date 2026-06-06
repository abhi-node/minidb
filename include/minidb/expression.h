#ifndef MINIDB_EXPRESSION_H
#define MINIDB_EXPRESSION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "minidb/schema.h"

typedef enum {
    EQUALS = 0,
    LESS,
    GREATER,
    PLUS,
    MINUS,
    MULTIPLY,
    DIVIDE,
    AND,
    OR
} minidb_operation_t;

typedef enum {
    NODE_EXPRESSION = 0,
    NODE_COLUMN,
    NODE_INT,
    NODE_DECIMAL,
    NODE_VARCHAR
} minidb_node_t;

typedef struct minidb_expr_t minidb_expr_t;

struct minidb_expr_t {
    minidb_node_t type;
    union {
        int32_t number;
        double decimal;
        struct {
            char *data;
            size_t length;
        } varchar;
        struct {
            char *data;
            size_t length;
        } column;
        struct {
            minidb_operation_t op;
            minidb_expr_t *l_expr;
            minidb_expr_t *r_expr;
        } expr;
    } data;

    minidb_data_t (*eval_expr)(minidb_expr_t *expr, minidb_structured_row_t *row);
};

minidb_expr_t make_int(int32_t number);
minidb_expr_t make_decimal(double decimal);
minidb_expr_t make_varchar(size_t length, char* data);
minidb_expr_t make_column(size_t length, char* data);
minidb_expr_t make_expr(minidb_operation_t op, minidb_expr_t *l_expr, minidb_expr_t *r_expr);

minidb_data_t eval_int(minidb_expr_t *expr, minidb_structured_row_t *row);
minidb_data_t eval_decimal(minidb_expr_t *expr, minidb_structured_row_t *row);
minidb_data_t eval_varchar(minidb_expr_t *expr, minidb_structured_row_t *row);
minidb_data_t eval_column(minidb_expr_t *expr, minidb_structured_row_t *row);
minidb_data_t eval_expr(minidb_expr_t *expr, minidb_structured_row_t *row);

#endif
