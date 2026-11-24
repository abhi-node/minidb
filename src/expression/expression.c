#include "minidb/expression.h"

#include <string.h>

/* ---- Binding ------------------------------------------------------------- */

static const char *binary_op_symbol(minidb_binary_op_t op) {
    switch (op) {
        case MINIDB_OP_ADD:           return "+";
        case MINIDB_OP_SUBTRACT:      return "-";
        case MINIDB_OP_MULTIPLY:      return "*";
        case MINIDB_OP_DIVIDE:        return "/";
        case MINIDB_OP_EQUAL:         return "=";
        case MINIDB_OP_NOT_EQUAL:     return "!=";
        case MINIDB_OP_LESS:          return "<";
        case MINIDB_OP_LESS_EQUAL:    return "<=";
        case MINIDB_OP_GREATER:       return ">";
        case MINIDB_OP_GREATER_EQUAL: return ">=";
        case MINIDB_OP_AND:           return "AND";
        case MINIDB_OP_OR:            return "OR";
        case MINIDB_OP_LIKE:          return "LIKE";
    }
    return "?";
}

static minidb_status_t bind_column(minidb_expr_t *expr, const minidb_row_shape_t *shape,
                                   minidb_error_t *error) {
    const char *table = expr->as.column.table;
    const char *name = expr->as.column.name;
    uint32_t match_count = 0;

    for (uint32_t index = 0; shape != NULL && index < shape->field_count; index++) {
        const minidb_field_t *field = &shape->fields[index];
        if (strcmp(field->name, name) != 0) {
            continue;
        }
        if (table != NULL && (field->table == NULL || strcmp(field->table, table) != 0)) {
            continue;
        }
        if (match_count++ == 0) {
            expr->as.column.index = index;
            expr->type = field->type;
        }
    }

    if (match_count == 1) {
        return MINIDB_OK;
    }
    if (match_count > 1) {
        return minidb_error_set(error, MINIDB_ERR_UNKNOWN_COLUMN,
                                "column '%s' is ambiguous; qualify it with a table name", name);
    }
    if (table != NULL) {
        return minidb_error_set(error, MINIDB_ERR_UNKNOWN_COLUMN, "no such column '%s.%s'",
                                table, name);
    }
    return minidb_error_set(error, MINIDB_ERR_UNKNOWN_COLUMN, "no such column '%s'", name);
}

static minidb_status_t bind_unary(minidb_expr_t *expr, minidb_error_t *error) {
    minidb_type_t operand_type = expr->as.unary.operand->type;

    if (expr->as.unary.op == MINIDB_OP_NOT) {
        if (operand_type != MINIDB_TYPE_BOOL) {
            return minidb_error_set(error, MINIDB_ERR_TYPE_MISMATCH,
                                    "NOT needs a BOOL, not %s", minidb_type_name(operand_type));
        }
        expr->type = MINIDB_TYPE_BOOL;
        return MINIDB_OK;
    }
    if (!minidb_type_is_numeric(operand_type)) {
        return minidb_error_set(error, MINIDB_ERR_TYPE_MISMATCH, "cannot negate %s",
                                minidb_type_name(operand_type));
    }
    expr->type = operand_type;
    return MINIDB_OK;
}

static minidb_status_t bind_binary(minidb_expr_t *expr, minidb_error_t *error) {
    minidb_binary_op_t op = expr->as.binary.op;
    minidb_type_t left = expr->as.binary.left->type;
    minidb_type_t right = expr->as.binary.right->type;
    bool is_well_typed = false;

    switch (op) {
        case MINIDB_OP_ADD:
        case MINIDB_OP_SUBTRACT:
        case MINIDB_OP_MULTIPLY:
        case MINIDB_OP_DIVIDE:
            is_well_typed = minidb_type_is_numeric(left) && minidb_type_is_numeric(right);
            /* INT op INT stays INT; anything involving a DECIMAL is a DECIMAL. */
            expr->type = (left == MINIDB_TYPE_INT && right == MINIDB_TYPE_INT)
                             ? MINIDB_TYPE_INT
                             : MINIDB_TYPE_DECIMAL;
            break;

        case MINIDB_OP_EQUAL:
        case MINIDB_OP_NOT_EQUAL:
            is_well_typed = minidb_types_comparable(left, right);
            expr->type = MINIDB_TYPE_BOOL;
            break;

        case MINIDB_OP_LESS:
        case MINIDB_OP_LESS_EQUAL:
        case MINIDB_OP_GREATER:
        case MINIDB_OP_GREATER_EQUAL:
            /* Booleans can be tested for equality but have no order. */
            is_well_typed = minidb_types_comparable(left, right) && left != MINIDB_TYPE_BOOL;
            expr->type = MINIDB_TYPE_BOOL;
            break;

        case MINIDB_OP_AND:
        case MINIDB_OP_OR:
            is_well_typed = left == MINIDB_TYPE_BOOL && right == MINIDB_TYPE_BOOL;
            expr->type = MINIDB_TYPE_BOOL;
            break;

        case MINIDB_OP_LIKE:
            is_well_typed = left == MINIDB_TYPE_TEXT && right == MINIDB_TYPE_TEXT;
            expr->type = MINIDB_TYPE_BOOL;
            break;
    }

    if (!is_well_typed) {
        return minidb_error_set(error, MINIDB_ERR_TYPE_MISMATCH, "cannot apply %s to %s and %s",
                                binary_op_symbol(op), minidb_type_name(left),
                                minidb_type_name(right));
    }
    return MINIDB_OK;
}

minidb_status_t minidb_expr_bind(minidb_expr_t *expr, const minidb_row_shape_t *shape,
                                 minidb_error_t *error) {
    minidb_status_t status;
    switch (expr->kind) {
        case MINIDB_EXPR_LITERAL:
            expr->type = expr->as.literal.type;
            return MINIDB_OK;

        case MINIDB_EXPR_COLUMN:
            return bind_column(expr, shape, error);

        case MINIDB_EXPR_UNARY:
            status = minidb_expr_bind(expr->as.unary.operand, shape, error);
            return status == MINIDB_OK ? bind_unary(expr, error) : status;

        case MINIDB_EXPR_BINARY:
            status = minidb_expr_bind(expr->as.binary.left, shape, error);
            if (status == MINIDB_OK) {
                status = minidb_expr_bind(expr->as.binary.right, shape, error);
            }
            return status == MINIDB_OK ? bind_binary(expr, error) : status;
    }
    return MINIDB_ERR_INVALID_ARGUMENT;
}

minidb_status_t minidb_expr_bind_predicate(minidb_expr_t *expr, const minidb_row_shape_t *shape,
                                           const char *clause, minidb_error_t *error) {
    minidb_status_t status = minidb_expr_bind(expr, shape, error);
    if (status == MINIDB_OK && expr->type != MINIDB_TYPE_BOOL) {
        return minidb_error_set(error, MINIDB_ERR_TYPE_MISMATCH,
                                "%s must be a BOOL condition, not %s", clause,
                                minidb_type_name(expr->type));
    }
    return status;
}

/* ---- Evaluation ---------------------------------------------------------- */

/*
 * SQL LIKE: '%' matches any run of bytes, '_' matches exactly one. Iterative
 * with a single backtrack point (the most recent '%'), so it is linear in
 * practice and cannot recurse.
 */
static bool like_matches(const char *text, uint32_t text_length, const char *pattern,
                         uint32_t pattern_length) {
    uint32_t text_index = 0;
    uint32_t pattern_index = 0;
    uint32_t wildcard_pattern_index = UINT32_MAX;  /* position after the last '%' */
    uint32_t wildcard_text_index = 0;              /* where that '%' currently ends */

    while (text_index < text_length) {
        if (pattern_index < pattern_length && pattern[pattern_index] == '%') {
            wildcard_pattern_index = ++pattern_index;
            wildcard_text_index = text_index;
        } else if (pattern_index < pattern_length &&
                   (pattern[pattern_index] == '_' ||
                    pattern[pattern_index] == text[text_index])) {
            pattern_index++;
            text_index++;
        } else if (wildcard_pattern_index != UINT32_MAX) {
            /* Mismatch: let the last '%' swallow one more byte and retry. */
            pattern_index = wildcard_pattern_index;
            text_index = ++wildcard_text_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern_length && pattern[pattern_index] == '%') {
        pattern_index++;
    }
    return pattern_index == pattern_length;
}

static double as_double(const minidb_value_t *value) {
    return value->type == MINIDB_TYPE_INT ? (double)value->as.integer : value->as.decimal;
}

/* Integer arithmetic wraps on overflow (computed unsigned to stay defined). */
static minidb_status_t evaluate_int_arithmetic(minidb_binary_op_t op, int64_t left,
                                               int64_t right, minidb_value_t *out_value) {
    uint64_t unsigned_left = (uint64_t)left;
    uint64_t unsigned_right = (uint64_t)right;
    switch (op) {
        case MINIDB_OP_ADD:
            *out_value = minidb_value_int((int64_t)(unsigned_left + unsigned_right));
            return MINIDB_OK;
        case MINIDB_OP_SUBTRACT:
            *out_value = minidb_value_int((int64_t)(unsigned_left - unsigned_right));
            return MINIDB_OK;
        case MINIDB_OP_MULTIPLY:
            *out_value = minidb_value_int((int64_t)(unsigned_left * unsigned_right));
            return MINIDB_OK;
        default:
            if (right == 0) {
                return MINIDB_ERR_DIVISION_BY_ZERO;
            }
            /* INT64_MIN / -1 overflows; it wraps back to INT64_MIN. */
            *out_value = minidb_value_int(right == -1 ? (int64_t)(0 - unsigned_left)
                                                      : left / right);
            return MINIDB_OK;
    }
}

static minidb_status_t evaluate_decimal_arithmetic(minidb_binary_op_t op, double left,
                                                   double right, minidb_value_t *out_value) {
    switch (op) {
        case MINIDB_OP_ADD:      *out_value = minidb_value_decimal(left + right); break;
        case MINIDB_OP_SUBTRACT: *out_value = minidb_value_decimal(left - right); break;
        case MINIDB_OP_MULTIPLY: *out_value = minidb_value_decimal(left * right); break;
        default:
            if (right == 0.0) {
                return MINIDB_ERR_DIVISION_BY_ZERO;
            }
            *out_value = minidb_value_decimal(left / right);
            break;
    }
    return MINIDB_OK;
}

static bool comparison_holds(minidb_binary_op_t op, int order) {
    switch (op) {
        case MINIDB_OP_EQUAL:         return order == 0;
        case MINIDB_OP_NOT_EQUAL:     return order != 0;
        case MINIDB_OP_LESS:          return order < 0;
        case MINIDB_OP_LESS_EQUAL:    return order <= 0;
        case MINIDB_OP_GREATER:       return order > 0;
        default:                      return order >= 0;
    }
}

static minidb_status_t evaluate_binary(const minidb_expr_t *expr, const minidb_value_t *row,
                                       minidb_value_t *out_value) {
    minidb_binary_op_t op = expr->as.binary.op;
    minidb_value_t left;
    minidb_value_t right;

    minidb_status_t status = minidb_expr_evaluate(expr->as.binary.left, row, &left);
    if (status != MINIDB_OK) {
        return status;
    }

    /* AND / OR skip the right side when the left side already decides. */
    if (op == MINIDB_OP_AND || op == MINIDB_OP_OR) {
        bool left_decides = left.as.boolean == (op == MINIDB_OP_OR);
        if (left_decides) {
            *out_value = left;
            return MINIDB_OK;
        }
        return minidb_expr_evaluate(expr->as.binary.right, row, out_value);
    }

    status = minidb_expr_evaluate(expr->as.binary.right, row, &right);
    if (status != MINIDB_OK) {
        return status;
    }

    switch (op) {
        case MINIDB_OP_ADD:
        case MINIDB_OP_SUBTRACT:
        case MINIDB_OP_MULTIPLY:
        case MINIDB_OP_DIVIDE:
            if (expr->type == MINIDB_TYPE_INT) {
                return evaluate_int_arithmetic(op, left.as.integer, right.as.integer, out_value);
            }
            return evaluate_decimal_arithmetic(op, as_double(&left), as_double(&right),
                                               out_value);
        case MINIDB_OP_LIKE:
            *out_value = minidb_value_bool(like_matches(left.as.text.bytes, left.as.text.length,
                                                        right.as.text.bytes,
                                                        right.as.text.length));
            return MINIDB_OK;
        default:
            *out_value = minidb_value_bool(comparison_holds(op, minidb_value_compare(&left, &right)));
            return MINIDB_OK;
    }
}

minidb_status_t minidb_expr_evaluate(const minidb_expr_t *expr, const minidb_value_t *row,
                                     minidb_value_t *out_value) {
    switch (expr->kind) {
        case MINIDB_EXPR_LITERAL:
            *out_value = expr->as.literal;
            return MINIDB_OK;

        case MINIDB_EXPR_COLUMN:
            *out_value = row[expr->as.column.index];
            return MINIDB_OK;

        case MINIDB_EXPR_UNARY: {
            minidb_status_t status = minidb_expr_evaluate(expr->as.unary.operand, row, out_value);
            if (status != MINIDB_OK) {
                return status;
            }
            if (expr->as.unary.op == MINIDB_OP_NOT) {
                out_value->as.boolean = !out_value->as.boolean;
            } else if (out_value->type == MINIDB_TYPE_INT) {
                out_value->as.integer = (int64_t)(0 - (uint64_t)out_value->as.integer);
            } else {
                out_value->as.decimal = -out_value->as.decimal;
            }
            return MINIDB_OK;
        }

        case MINIDB_EXPR_BINARY:
            return evaluate_binary(expr, row, out_value);
    }
    return MINIDB_ERR_INVALID_ARGUMENT;
}
