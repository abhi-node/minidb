#include "minidb/operator.h"

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *child;
    int64_t row_limit;
    int64_t returned_count;
} limit_t;

static minidb_status_t limit_open(minidb_operator_t *self) {
    limit_t *limit = (limit_t *)self;
    limit->returned_count = 0;
    return minidb_operator_open(limit->child);
}

static minidb_status_t limit_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    limit_t *limit = (limit_t *)self;
    if (limit->returned_count >= limit->row_limit) {
        return MINIDB_DONE;  /* stop pulling: the child does no further work */
    }
    minidb_status_t status = minidb_operator_next(limit->child, out_row);
    if (status == MINIDB_OK) {
        limit->returned_count++;
    }
    return status;
}

static void limit_close(minidb_operator_t *self) {
    minidb_operator_close(((limit_t *)self)->child);
}

static const minidb_operator_vtable_t LIMIT_VTABLE = {"Limit", limit_open, limit_next,
                                                      limit_close};

minidb_status_t minidb_limit_create(minidb_arena_t *arena, minidb_operator_t *child,
                                    int64_t row_limit, minidb_operator_t **out_limit) {
    limit_t *limit = minidb_arena_alloc_zeroed(arena, sizeof(*limit));
    if (limit == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    limit->base.vtable = &LIMIT_VTABLE;
    limit->base.shape = child->shape;
    limit->child = child;
    limit->row_limit = row_limit;
    *out_limit = &limit->base;
    return MINIDB_OK;
}
