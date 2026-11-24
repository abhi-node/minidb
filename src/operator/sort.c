#include <stdlib.h>
#include <string.h>

#include "minidb/operator.h"

/*
 * Sort has to see every row before it can return the first, so open() drains
 * the child into memory. Rows are deep-copied (TEXT included) into a private
 * arena, because the child's rows are only valid until its next call.
 */

typedef struct {
    minidb_operator_t base;
    minidb_operator_t *child;
    const minidb_order_term_t *terms;
    uint32_t term_count;

    minidb_arena_t row_arena;  /* owns the copied rows; freed in close() */
    minidb_value_t **rows;     /* malloc'd array of pointers into row_arena */
    size_t row_count;
    size_t row_capacity;
    size_t next_row;           /* next row to return */

    minidb_status_t compare_status;  /* first error hit while comparing, if any */
} sort_t;

static minidb_status_t sort_remember_row(sort_t *sort, const minidb_value_t *row) {
    uint32_t column_count = sort->base.shape.field_count;

    if (sort->row_count == sort->row_capacity) {
        size_t capacity = sort->row_capacity == 0 ? 256 : sort->row_capacity * 2;
        minidb_value_t **rows = realloc(sort->rows, capacity * sizeof(*rows));
        if (rows == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        sort->rows = rows;
        sort->row_capacity = capacity;
    }

    minidb_value_t *copy = minidb_arena_alloc(&sort->row_arena, column_count * sizeof(*copy));
    if (copy == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    memcpy(copy, row, column_count * sizeof(*copy));
    for (uint32_t column = 0; column < column_count; column++) {
        if (copy[column].type != MINIDB_TYPE_TEXT) {
            continue;
        }
        char *text = minidb_arena_alloc(&sort->row_arena, copy[column].as.text.length);
        if (text == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        memcpy(text, copy[column].as.text.bytes, copy[column].as.text.length);
        copy[column].as.text.bytes = text;
    }
    sort->rows[sort->row_count++] = copy;
    return MINIDB_OK;
}

/* True if `left` must come before or may stay before `right`. */
static bool sort_in_order(sort_t *sort, const minidb_value_t *left, const minidb_value_t *right) {
    for (uint32_t index = 0; index < sort->term_count; index++) {
        const minidb_order_term_t *term = &sort->terms[index];
        minidb_value_t left_key;
        minidb_value_t right_key;

        minidb_status_t status = minidb_expr_evaluate(term->expr, left, &left_key);
        if (status == MINIDB_OK) {
            status = minidb_expr_evaluate(term->expr, right, &right_key);
        }
        if (status != MINIDB_OK) {
            if (sort->compare_status == MINIDB_OK) {
                sort->compare_status = status;
            }
            return true;
        }

        int order = minidb_value_compare(&left_key, &right_key);
        if (order != 0) {
            return term->is_descending ? order > 0 : order < 0;
        }
    }
    return true;  /* equal on every term: keep the input order (stable) */
}

/* Top-down merge sort of rows[begin, end) using `scratch` as the merge buffer. */
static void sort_merge_sort(sort_t *sort, minidb_value_t **scratch, size_t begin, size_t end) {
    if (end - begin < 2) {
        return;
    }
    size_t middle = begin + (end - begin) / 2;
    sort_merge_sort(sort, scratch, begin, middle);
    sort_merge_sort(sort, scratch, middle, end);

    size_t left = begin;
    size_t right = middle;
    for (size_t out = begin; out < end; out++) {
        bool take_left = right == end ||
                         (left < middle && sort_in_order(sort, sort->rows[left], sort->rows[right]));
        scratch[out] = take_left ? sort->rows[left++] : sort->rows[right++];
    }
    memcpy(sort->rows + begin, scratch + begin, (end - begin) * sizeof(*scratch));
}

static void sort_release_rows(sort_t *sort) {
    free(sort->rows);
    sort->rows = NULL;
    sort->row_count = 0;
    sort->row_capacity = 0;
    sort->next_row = 0;
    minidb_arena_destroy(&sort->row_arena);
    minidb_arena_init(&sort->row_arena);
}

static minidb_status_t sort_open(minidb_operator_t *self) {
    sort_t *sort = (sort_t *)self;
    sort_release_rows(sort);
    sort->compare_status = MINIDB_OK;

    minidb_status_t status = minidb_operator_open(sort->child);
    const minidb_value_t *row;
    while (status == MINIDB_OK && (status = minidb_operator_next(sort->child, &row)) == MINIDB_OK) {
        status = sort_remember_row(sort, row);
    }
    /* Everything is copied; let the child drop its pinned pages now. */
    minidb_operator_close(sort->child);
    if (status != MINIDB_DONE) {
        return status;
    }

    if (sort->row_count > 1) {
        minidb_value_t **scratch = malloc(sort->row_count * sizeof(*scratch));
        if (scratch == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        sort_merge_sort(sort, scratch, 0, sort->row_count);
        free(scratch);
    }
    return sort->compare_status;
}

static minidb_status_t sort_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    sort_t *sort = (sort_t *)self;
    if (sort->next_row == sort->row_count) {
        return MINIDB_DONE;
    }
    *out_row = sort->rows[sort->next_row++];
    return MINIDB_OK;
}

static void sort_close(minidb_operator_t *self) {
    sort_t *sort = (sort_t *)self;
    sort_release_rows(sort);
    minidb_operator_close(sort->child);
}

static const minidb_operator_vtable_t SORT_VTABLE = {"Sort", sort_open, sort_next, sort_close};

minidb_status_t minidb_sort_create(minidb_arena_t *arena, minidb_operator_t *child,
                                   const minidb_order_term_t *terms, uint32_t term_count,
                                   minidb_operator_t **out_sort) {
    sort_t *sort = minidb_arena_alloc_zeroed(arena, sizeof(*sort));
    if (sort == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    sort->base.vtable = &SORT_VTABLE;
    sort->base.shape = child->shape;
    sort->child = child;
    sort->terms = terms;
    sort->term_count = term_count;
    minidb_arena_init(&sort->row_arena);
    *out_sort = &sort->base;
    return MINIDB_OK;
}
