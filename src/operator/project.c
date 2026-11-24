#include "minidb/operator.h"

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *child;
    minidb_expr_t **exprs;   /* one per output column */
    minidb_value_t *values;  /* current output row; TEXT borrows from the child's row */
} project_t;

static minidb_status_t project_open(minidb_operator_t *self) {
    return minidb_operator_open(((project_t *)self)->child);
}

static minidb_status_t project_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    project_t *project = (project_t *)self;

    const minidb_value_t *child_row;
    minidb_status_t status = minidb_operator_next(project->child, &child_row);
    if (status != MINIDB_OK) {
        return status;
    }
    for (uint32_t column = 0; column < project->base.shape.field_count; column++) {
        status = minidb_expr_evaluate(project->exprs[column], child_row, &project->values[column]);
        if (status != MINIDB_OK) {
            return status;
        }
    }
    *out_row = project->values;
    return MINIDB_OK;
}

static void project_close(minidb_operator_t *self) {
    minidb_operator_close(((project_t *)self)->child);
}

static const minidb_operator_vtable_t PROJECT_VTABLE = {"Project", project_open, project_next,
                                                        project_close};

minidb_status_t minidb_project_create(minidb_arena_t *arena, minidb_operator_t *child,
                                      minidb_expr_t **exprs, minidb_field_t *fields,
                                      uint32_t column_count, minidb_operator_t **out_project) {
    project_t *project = minidb_arena_alloc_zeroed(arena, sizeof(*project));
    minidb_value_t *values = minidb_arena_alloc(arena, column_count * sizeof(*values));
    if (project == NULL || values == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    project->base.vtable = &PROJECT_VTABLE;
    project->base.shape.fields = fields;
    project->base.shape.field_count = column_count;
    project->child = child;
    project->exprs = exprs;
    project->values = values;
    *out_project = &project->base;
    return MINIDB_OK;
}
