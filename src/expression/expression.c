#include "minidb/expression.h"
#include "minidb/schema.h"
#include <string.h>

minidb_expr_t make_int(int32_t number) {
    minidb_expr_t expr;
    expr.type = NODE_INT;
    expr.data.number = number;

    expr.evaluate = eval_int;

    return expr;
}

minidb_expr_t make_decimal(double decimal) {
    minidb_expr_t expr;
    expr.type = NODE_DECIMAL;
    expr.data.decimal = decimal;

    expr.evaluate = eval_decimal;

    return expr;
}

minidb_expr_t make_varchar(size_t length, char *data) {
    minidb_expr_t expr;
    expr.type = NODE_VARCHAR;
    expr.data.varchar.length = length;
    expr.data.varchar.data = data;

    expr.evaluate = eval_varchar;

    return expr;
}

minidb_expr_t make_column(size_t length, char *data) {
    minidb_expr_t expr;
    expr.type = NODE_COLUMN;
    expr.data.varchar.length = length;
    expr.data.varchar.data = data;

    expr.evaluate = eval_column;

    return expr;
}

minidb_expr_t make_expr(minidb_operation_t op, minidb_expr_t *l_expr, minidb_expr_t *r_expr) {
    minidb_expr_t expr;
    expr.type = NODE_EXPRESSION;
    expr.data.expr.op = op;
    expr.data.expr.l_expr = l_expr;
    expr.data.expr.r_expr = r_expr;

    expr.evaluate = eval_expr;

    return expr;
}

minidb_data_t eval_int(minidb_expr_t *expr, minidb_structured_row_t *row) {
    return (minidb_data_t) {
        .type=INT_TYPE,
        .data.number = expr->data.number
    };
}

minidb_data_t eval_decimal(minidb_expr_t *expr, minidb_structured_row_t *row) {
    return (minidb_data_t) {
        .type=DECIMAL_TYPE,
        .data.decimal = expr->data.decimal
    };
}

minidb_data_t eval_varchar(minidb_expr_t *expr, minidb_structured_row_t *row) {
    return (minidb_data_t) {
        .type=VARCHAR_TYPE,
        .data.varchar.data = expr->data.varchar.data,
        .data.varchar.length = expr->data.varchar.length
    };
}

minidb_data_t eval_column(minidb_expr_t *expr, minidb_structured_row_t *row) {
    for (size_t i = 0; i < row->n_cols; i++) {
        if (strcmp(expr->data.column.data, row->cols[i].name)==0) {
            return row->data[i];
        }
    }
}

minidb_data_t eval_varchar_op(minidb_data_t *l, minidb_data_t *r, minidb_operation_t op) {
    if (op != EQUALS) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    return (minidb_data_t) {
        .type = BOOL_TYPE,
        .data.boolean = strcmp(l->data.varchar.data, r->data.varchar.data) == 0
    };
}

minidb_data_t eval_bool_op(minidb_data_t *l, minidb_data_t *r, minidb_operation_t op) {
    if (op != EQUALS && op != AND && op != OR) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    bool result = false;

    switch (op) {
        case EQUALS: {
            result = l->data.boolean == r->data.boolean;
        }
        case AND: {
            result = l->data.boolean && r->data.boolean;
        }
        case OR: {
            result = l->data.boolean || r->data.boolean;
        }
    }

    return (minidb_data_t) {
        .type = BOOL_TYPE,
        .data.boolean = result
    };
}

minidb_data_t eval_int_op(minidb_data_t *l, minidb_data_t *r, minidb_operation_t op) {
    if (op != EQUALS && op != LESS && op != GREATER && op != PLUS && op != MINUS && op != DIVIDE && op != MULTIPLY) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    switch (op) {
        case PLUS: {
            return (minidb_data_t) {
                .type = INT_TYPE,
                .data.number = l->data.number + r->data.number
            };
        }
        case MINUS: {
            return (minidb_data_t) {
                .type = INT_TYPE,
                .data.number = l->data.number - r->data.number
            };
        }
        case DIVIDE: {
            if (r->data.number == 0) {
                return (minidb_data_t) {
                    .type = BOOL_TYPE,
                    .data.boolean = false
                };
            }

            return (minidb_data_t) {
                .type = INT_TYPE,
                .data.number = l->data.number / r->data.number
            };
        }
        case MULTIPLY: {
            return (minidb_data_t) {
                .type = INT_TYPE,
                .data.number = l->data.number * r->data.number
            };
        }
        case EQUALS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.number == r->data.number
            };
        }
        case GREATER: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.number > r->data.number
            };
        }
        case LESS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.number < r->data.number
            };
        }
    }

    return (minidb_data_t) {
        .type = BOOL_TYPE,
        .data.boolean = false
    };
}

minidb_data_t eval_decimal_op(minidb_data_t *l, minidb_data_t *r, minidb_operation_t op) {
    if (op != EQUALS && op != LESS && op != GREATER && op != PLUS && op != MINUS && op != DIVIDE && op != MULTIPLY) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    switch (op) {
        case PLUS: {
            return (minidb_data_t) {
                .type = DECIMAL_TYPE,
                .data.decimal = l->data.decimal + r->data.decimal
            };
        }
        case MINUS: {
            return (minidb_data_t) {
                .type = DECIMAL_TYPE,
                .data.decimal = l->data.decimal - r->data.decimal
            };
        }
        case DIVIDE: {
            if (r->data.decimal == 0) {
                return (minidb_data_t) {
                    .type = BOOL_TYPE,
                    .data.boolean = false
                };
            }

            return (minidb_data_t) {
                .type = DECIMAL_TYPE,
                .data.decimal = l->data.decimal / r->data.decimal
            };
        }
        case MULTIPLY: {
            return (minidb_data_t) {
                .type = DECIMAL_TYPE,
                .data.decimal = l->data.decimal * r->data.decimal
            };
        }
        case EQUALS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.decimal == r->data.decimal
            };
        }
        case GREATER: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.decimal > r->data.decimal
            };
        }
        case LESS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.decimal < r->data.decimal
            };
        }
    }

    return (minidb_data_t) {
        .type = BOOL_TYPE,
        .data.boolean = false
    };
}

minidb_data_t eval_id_op(minidb_data_t *l, minidb_data_t *r, minidb_operation_t op) {
    if (op != EQUALS && op != LESS && op != GREATER && op != PLUS && op != MINUS && op != DIVIDE && op != MULTIPLY) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    switch (op) {
        case PLUS: {
            return (minidb_data_t) {
                .type = ID_TYPE,
                .data.id = l->data.id + r->data.id
            };
        }
        case MINUS: {
            return (minidb_data_t) {
                .type = ID_TYPE,
                .data.id = l->data.id - r->data.id
            };
        }
        case DIVIDE: {
            if (r->data.number == 0) {
                return (minidb_data_t) {
                    .type = BOOL_TYPE,
                    .data.boolean = false
                };
            }

            return (minidb_data_t) {
                .type = ID_TYPE,
                .data.id = l->data.id / r->data.id
            };
        }
        case MULTIPLY: {
            return (minidb_data_t) {
                .type = ID_TYPE,
                .data.id = l->data.id * r->data.id
            };
        }
        case EQUALS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.id == r->data.id
            };
        }
        case GREATER: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.id > r->data.id
            };
        }
        case LESS: {
            return (minidb_data_t) {
                .type = BOOL_TYPE,
                .data.boolean = l->data.id < r->data.id
            };
        }
    }

    return (minidb_data_t) {
        .type = BOOL_TYPE,
        .data.boolean = false
    };
}

minidb_data_t eval_expr(minidb_expr_t *expr, minidb_structured_row_t *row) {
    minidb_data_t left_data = expr->data.expr.l_expr->evaluate(expr->data.expr.l_expr, row);
    minidb_data_t right_data = expr->data.expr.r_expr->evaluate(expr->data.expr.r_expr, row);
    minidb_operation_t op = expr->data.expr.op;

    if (left_data.type != right_data.type) {
        return (minidb_data_t) {
            .type = BOOL_TYPE,
            .data.boolean = false
        };
    }

    switch (left_data.type) {
        case VARCHAR_TYPE: {
            return eval_varchar_op(&left_data, &right_data, op);
        }
        case BOOL_TYPE: {
            return eval_bool_op(&left_data, &right_data, op);
        }
        case INT_TYPE: {
            return eval_int_op(&left_data, &right_data, op);
        }
        case DECIMAL_TYPE: {
            return eval_decimal_op(&left_data, &right_data, op);
        }
        case ID_TYPE: {
            return eval_id_op(&left_data, &right_data, op);
        }
    }
}
