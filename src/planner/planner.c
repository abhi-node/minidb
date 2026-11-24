#include "minidb/planner.h"

#include <string.h>

/* ---- Primary-key ranges -------------------------------------------------- */

typedef struct {
    int64_t first;  /* inclusive; the range is empty when first > last */
    int64_t last;
} key_range_t;

static const key_range_t FULL_KEY_RANGE = {INT64_MIN, INT64_MAX};
static const key_range_t EMPTY_KEY_RANGE = {INT64_MAX, INT64_MIN};

static bool is_key_column(const minidb_expr_t *expr, uint32_t key_column) {
    return expr->kind == MINIDB_EXPR_COLUMN && expr->as.column.index == key_column;
}

static bool is_int_literal(const minidb_expr_t *expr) {
    return expr->kind == MINIDB_EXPR_LITERAL && expr->as.literal.type == MINIDB_TYPE_INT;
}

/* The operator that means the same with its operands swapped: 5 < id is id > 5. */
static minidb_binary_op_t mirrored_comparison(minidb_binary_op_t op) {
    switch (op) {
        case MINIDB_OP_LESS:          return MINIDB_OP_GREATER;
        case MINIDB_OP_LESS_EQUAL:    return MINIDB_OP_GREATER_EQUAL;
        case MINIDB_OP_GREATER:       return MINIDB_OP_LESS;
        case MINIDB_OP_GREATER_EQUAL: return MINIDB_OP_LESS_EQUAL;
        default:                      return op;
    }
}

/* Shrinks `range` to the keys that can satisfy `key <op> bound`. */
static void apply_key_bound(minidb_binary_op_t op, int64_t bound, key_range_t *range) {
    key_range_t allowed = FULL_KEY_RANGE;
    switch (op) {
        case MINIDB_OP_EQUAL:
            allowed.first = bound;
            allowed.last = bound;
            break;
        case MINIDB_OP_LESS:
            allowed = bound == INT64_MIN ? EMPTY_KEY_RANGE : (key_range_t){INT64_MIN, bound - 1};
            break;
        case MINIDB_OP_LESS_EQUAL:
            allowed.last = bound;
            break;
        case MINIDB_OP_GREATER:
            allowed = bound == INT64_MAX ? EMPTY_KEY_RANGE : (key_range_t){bound + 1, INT64_MAX};
            break;
        case MINIDB_OP_GREATER_EQUAL:
            allowed.first = bound;
            break;
        default:
            return;  /* != and the rest say nothing useful about a range */
    }
    if (allowed.first > range->first) {
        range->first = allowed.first;
    }
    if (allowed.last < range->last) {
        range->last = allowed.last;
    }
}

/*
 * Narrows `range` using the conjuncts of `predicate` that compare the key
 * column with an integer constant. Only AND is descended into: every row
 * that passes must satisfy both sides, so each side may narrow the range.
 * Anything else is left for the filter to check row by row.
 */
static void narrow_key_range(const minidb_expr_t *predicate, uint32_t key_column,
                             key_range_t *range) {
    if (predicate->kind != MINIDB_EXPR_BINARY) {
        return;
    }
    const minidb_expr_t *left = predicate->as.binary.left;
    const minidb_expr_t *right = predicate->as.binary.right;
    minidb_binary_op_t op = predicate->as.binary.op;

    if (op == MINIDB_OP_AND) {
        narrow_key_range(left, key_column, range);
        narrow_key_range(right, key_column, range);
    } else if (is_key_column(left, key_column) && is_int_literal(right)) {
        apply_key_bound(op, right->as.literal.as.integer, range);
    } else if (is_int_literal(left) && is_key_column(right, key_column)) {
        apply_key_bound(mirrored_comparison(op), left->as.literal.as.integer, range);
    }
}

/* Applies WHERE's key bounds to a scan of `table` whose columns start at 0. */
static void push_key_range_into_scan(minidb_operator_t *scan, const minidb_table_t *table,
                                     const minidb_expr_t *where) {
    if (where == NULL || table->schema.primary_key_index == MINIDB_NO_PRIMARY_KEY) {
        return;
    }
    key_range_t range = FULL_KEY_RANGE;
    narrow_key_range(where, (uint32_t)table->schema.primary_key_index, &range);
    minidb_scan_set_key_range(scan, range.first, range.last);
}

/* ---- Joins --------------------------------------------------------------- */

/* True if every column `expr` reads lies in the first `column_limit` columns. */
static bool reads_only_columns_below(const minidb_expr_t *expr, uint32_t column_limit) {
    switch (expr->kind) {
        case MINIDB_EXPR_LITERAL:
            return true;
        case MINIDB_EXPR_COLUMN:
            return expr->as.column.index < column_limit;
        case MINIDB_EXPR_UNARY:
            return reads_only_columns_below(expr->as.unary.operand, column_limit);
        case MINIDB_EXPR_BINARY:
            return reads_only_columns_below(expr->as.binary.left, column_limit) &&
                   reads_only_columns_below(expr->as.binary.right, column_limit);
    }
    return false;
}

/*
 * Looks through the AND-ed parts of a join condition for
 * `<right primary key> = <INT expression over left columns>` (either way
 * round). Returns that conjunct, with *out_key_expr set to the left-side
 * expression, or NULL if there is none.
 */
static const minidb_expr_t *find_index_join_key(const minidb_expr_t *condition,
                                                uint32_t left_column_count,
                                                uint32_t right_key_column,
                                                const minidb_expr_t **out_key_expr) {
    if (condition->kind != MINIDB_EXPR_BINARY) {
        return NULL;
    }
    const minidb_expr_t *left = condition->as.binary.left;
    const minidb_expr_t *right = condition->as.binary.right;

    if (condition->as.binary.op == MINIDB_OP_AND) {
        const minidb_expr_t *found =
            find_index_join_key(left, left_column_count, right_key_column, out_key_expr);
        if (found == NULL) {
            found = find_index_join_key(right, left_column_count, right_key_column, out_key_expr);
        }
        return found;
    }
    if (condition->as.binary.op != MINIDB_OP_EQUAL) {
        return NULL;
    }

    const minidb_expr_t *key_expr = NULL;
    if (is_key_column(left, right_key_column)) {
        key_expr = right;
    } else if (is_key_column(right, right_key_column)) {
        key_expr = left;
    }
    if (key_expr == NULL || key_expr->type != MINIDB_TYPE_INT ||
        !reads_only_columns_below(key_expr, left_column_count)) {
        return NULL;
    }
    *out_key_expr = key_expr;
    return condition;
}

static minidb_status_t find_table(minidb_storage_t *storage, const char *table_name,
                                  minidb_table_t **out_table, minidb_error_t *error) {
    *out_table = minidb_catalog_find(minidb_storage_catalog(storage), table_name);
    if (*out_table == NULL) {
        return minidb_error_set(error, MINIDB_ERR_UNKNOWN_TABLE, "no such table '%s'",
                                table_name);
    }
    return MINIDB_OK;
}

/* Joins `*plan` (the rows so far) with one more table. */
static minidb_status_t plan_join(minidb_storage_t *storage, minidb_arena_t *arena,
                                 minidb_join_t *join, minidb_operator_t **plan,
                                 minidb_error_t *error) {
    minidb_pager_t *pager = minidb_storage_pager(storage);
    minidb_operator_t *left = *plan;
    uint32_t left_column_count = left->shape.field_count;

    for (uint32_t index = 0; index < left_column_count; index++) {
        const char *used_alias = left->shape.fields[index].table;
        if (used_alias != NULL && strcmp(used_alias, join->table.alias) == 0) {
            return minidb_error_set(error, MINIDB_ERR_UNSUPPORTED,
                                    "table '%s' appears twice; give one of them an alias",
                                    join->table.alias);
        }
    }

    minidb_table_t *right_table;
    minidb_status_t status = find_table(storage, join->table.name, &right_table, error);
    if (status != MINIDB_OK) {
        return status;
    }

    /* Start from the general plan: it defines the combined row shape that
     * the ON condition is bound against. */
    minidb_operator_t *right_scan;
    minidb_operator_t *nested_loop_join;
    status = minidb_scan_create(arena, pager, right_table, join->table.alias, &right_scan);
    if (status == MINIDB_OK) {
        status = minidb_nested_loop_join_create(arena, left, right_scan, join->condition,
                                                &nested_loop_join);
    }
    if (status == MINIDB_OK) {
        status = minidb_expr_bind_predicate(join->condition, &nested_loop_join->shape, "ON",
                                            error);
    }
    if (status != MINIDB_OK) {
        return status;
    }
    *plan = nested_loop_join;

    /* Upgrade to an index join when the condition pins the right table's key. */
    if (right_table->schema.primary_key_index == MINIDB_NO_PRIMARY_KEY) {
        return MINIDB_OK;
    }
    uint32_t right_key_column =
        left_column_count + (uint32_t)right_table->schema.primary_key_index;
    const minidb_expr_t *key_expr = NULL;
    const minidb_expr_t *key_conjunct =
        find_index_join_key(join->condition, left_column_count, right_key_column, &key_expr);
    if (key_conjunct == NULL) {
        return MINIDB_OK;
    }

    status = minidb_index_join_create(arena, left, pager, right_table, join->table.alias,
                                      key_expr, plan);
    if (status == MINIDB_OK && key_conjunct != join->condition) {
        /* The lookup covers one conjunct; a filter checks the others. */
        status = minidb_filter_create(arena, *plan, join->condition, plan);
    }
    return status;
}

/* ---- SELECT -------------------------------------------------------------- */

/* Adds the Project operator for an explicit select list. */
static minidb_status_t plan_projection(minidb_arena_t *arena, minidb_select_statement_t *select,
                                       minidb_operator_t **plan, minidb_error_t *error) {
    uint32_t column_count = select->item_count;
    minidb_expr_t **exprs = minidb_arena_alloc(arena, column_count * sizeof(*exprs));
    minidb_field_t *fields = minidb_arena_alloc(arena, column_count * sizeof(*fields));
    if (exprs == NULL || fields == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }

    for (uint32_t column = 0; column < column_count; column++) {
        const minidb_select_item_t *item = &select->items[column];
        minidb_status_t status = minidb_expr_bind(item->expr, &(*plan)->shape, error);
        if (status != MINIDB_OK) {
            return status;
        }

        /* A bare column keeps its name and table; anything else is computed. */
        bool is_plain_column = item->expr->kind == MINIDB_EXPR_COLUMN;
        const minidb_field_t *source =
            is_plain_column ? &(*plan)->shape.fields[item->expr->as.column.index] : NULL;

        exprs[column] = item->expr;
        fields[column].table = is_plain_column ? source->table : NULL;
        fields[column].name =
            item->alias != NULL ? item->alias : is_plain_column ? source->name : "?column?";
        fields[column].type = item->expr->type;
    }
    return minidb_project_create(arena, *plan, exprs, fields, column_count, plan);
}

minidb_status_t minidb_plan_select(minidb_storage_t *storage, minidb_arena_t *arena,
                                   minidb_select_statement_t *select,
                                   minidb_operator_t **out_plan, minidb_error_t *error) {
    minidb_table_t *from_table;
    minidb_status_t status = find_table(storage, select->from.name, &from_table, error);
    if (status != MINIDB_OK) {
        return status;
    }

    minidb_operator_t *from_scan;
    status = minidb_scan_create(arena, minidb_storage_pager(storage), from_table,
                                select->from.alias, &from_scan);
    minidb_operator_t *plan = from_scan;

    for (uint32_t index = 0; status == MINIDB_OK && index < select->join_count; index++) {
        status = plan_join(storage, arena, &select->joins[index], &plan, error);
    }

    if (status == MINIDB_OK && select->where != NULL) {
        status = minidb_expr_bind_predicate(select->where, &plan->shape, "WHERE", error);
        if (status == MINIDB_OK) {
            /* The FROM table's columns come first in every row shape, so its
             * key bounds can be read straight out of the bound WHERE. */
            push_key_range_into_scan(from_scan, from_table, select->where);
            status = minidb_filter_create(arena, plan, select->where, &plan);
        }
    }

    if (status == MINIDB_OK && select->order_term_count > 0) {
        for (uint32_t index = 0; status == MINIDB_OK && index < select->order_term_count;
             index++) {
            status = minidb_expr_bind(select->order_terms[index].expr, &plan->shape, error);
        }
        if (status == MINIDB_OK) {
            status = minidb_sort_create(arena, plan, select->order_terms,
                                        select->order_term_count, &plan);
        }
    }

    if (status == MINIDB_OK && !select->selects_all_columns) {
        status = plan_projection(arena, select, &plan, error);
    }
    if (status == MINIDB_OK && select->has_limit) {
        status = minidb_limit_create(arena, plan, select->limit, &plan);
    }

    *out_plan = status == MINIDB_OK ? plan : NULL;
    return status;
}

/* ---- UPDATE / DELETE row source ------------------------------------------ */

minidb_status_t minidb_plan_table_scan(minidb_storage_t *storage, minidb_arena_t *arena,
                                       const minidb_table_t *table, minidb_expr_t *where,
                                       minidb_operator_t **out_scan, minidb_error_t *error) {
    minidb_status_t status = minidb_scan_create(arena, minidb_storage_pager(storage), table,
                                                table->schema.table_name, out_scan);
    if (status == MINIDB_OK && where != NULL) {
        status = minidb_expr_bind_predicate(where, &(*out_scan)->shape, "WHERE", error);
    }
    if (status == MINIDB_OK) {
        push_key_range_into_scan(*out_scan, table, where);
    }
    return status;
}
