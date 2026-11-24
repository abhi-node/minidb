#include "minidb/operator.h"

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *child;
    const minidb_expr_t *predicate;
} filter_t;

static minidb_status_t filter_open(minidb_operator_t *self) {
    return minidb_operator_open(((filter_t *)self)->child);
}

static minidb_status_t filter_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    filter_t *filter = (filter_t *)self;
    for (;;) {
        minidb_status_t status = minidb_operator_next(filter->child, out_row);
        if (status != MINIDB_OK) {
            return status;
        }
        minidb_value_t is_match;
        status = minidb_expr_evaluate(filter->predicate, *out_row, &is_match);
        if (status != MINIDB_OK) {
            return status;
        }
        if (is_match.as.boolean) {
            return MINIDB_OK;
        }
    }
}

static void filter_close(minidb_operator_t *self) {
    minidb_operator_close(((filter_t *)self)->child);
}

static const minidb_operator_vtable_t FILTER_VTABLE = {"Filter", filter_open, filter_next,
                                                       filter_close};

minidb_status_t minidb_filter_create(minidb_arena_t *arena, minidb_operator_t *child,
                                     const minidb_expr_t *predicate,
                                     minidb_operator_t **out_filter) {
    filter_t *filter = minidb_arena_alloc_zeroed(arena, sizeof(*filter));
    if (filter == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    filter->base.vtable = &FILTER_VTABLE;
    filter->base.shape = child->shape;  /* a filter passes rows through unchanged */
    filter->child = child;
    filter->predicate = predicate;
    *out_filter = &filter->base;
    return MINIDB_OK;
}
