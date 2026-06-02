#ifndef MINIDB_AST_H
#define MINIDB_AST_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    MINIDB_EQUALS = 0,
    MINIDB_LESS,
    MINIDB_GREATER
} minidb_operation_t;

typedef enum {
    MINIDB_EXPR = 0,
    MINIDB_COL,
    MINIDB_NUMBER,
    MINIDB_DECIMAL,
    MINIDB_VARCHAR
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
};

#endif
