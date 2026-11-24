#include "minidb/btree.h"
#include "minidb/operator.h"
#include "minidb/row.h"

typedef struct {
    minidb_operator_t base;
    minidb_pager_t *pager;
    const minidb_table_t *table;
    int64_t first_key;  /* inclusive key range */
    int64_t last_key;
    minidb_cursor_t cursor;
    bool is_positioned;      /* false until the first next() after open() */
    int64_t current_key;
    minidb_value_t *values;  /* decoded current row; TEXT points into the pinned leaf */
} scan_t;

static minidb_status_t scan_open(minidb_operator_t *self) {
    scan_t *scan = (scan_t *)self;
    minidb_cursor_close(&scan->cursor);
    scan->is_positioned = false;
    return MINIDB_OK;
}

static minidb_status_t scan_next(minidb_operator_t *self, const minidb_value_t **out_row) {
    scan_t *scan = (scan_t *)self;

    minidb_status_t status;
    if (!scan->is_positioned) {
        if (scan->first_key > scan->last_key) {
            return MINIDB_DONE;
        }
        status = minidb_cursor_seek(&scan->cursor, scan->table->root_page, scan->first_key);
        scan->is_positioned = true;
    } else {
        status = minidb_cursor_is_valid(&scan->cursor) ? minidb_cursor_next(&scan->cursor)
                                                       : MINIDB_OK;
    }
    if (status != MINIDB_OK) {
        return status;
    }

    if (!minidb_cursor_is_valid(&scan->cursor) ||
        minidb_cursor_key(&scan->cursor) > scan->last_key) {
        minidb_cursor_close(&scan->cursor);
        return MINIDB_DONE;
    }

    uint32_t row_length;
    const uint8_t *row_bytes = minidb_cursor_payload(&scan->cursor, &row_length);
    status = minidb_row_decode(&scan->table->schema, row_bytes, row_length, scan->values);
    if (status != MINIDB_OK) {
        return status;
    }
    scan->current_key = minidb_cursor_key(&scan->cursor);
    *out_row = scan->values;
    return MINIDB_OK;
}

static void scan_close(minidb_operator_t *self) {
    scan_t *scan = (scan_t *)self;
    minidb_cursor_close(&scan->cursor);
}

static const minidb_operator_vtable_t SCAN_VTABLE = {"Scan", scan_open, scan_next, scan_close};

minidb_status_t minidb_scan_create(minidb_arena_t *arena, minidb_pager_t *pager,
                                   const minidb_table_t *table, const char *alias,
                                   minidb_operator_t **out_scan) {
    uint32_t column_count = table->schema.column_count;
    scan_t *scan = minidb_arena_alloc_zeroed(arena, sizeof(*scan));
    minidb_field_t *fields = minidb_arena_alloc(arena, column_count * sizeof(*fields));
    minidb_value_t *values = minidb_arena_alloc(arena, column_count * sizeof(*values));
    if (scan == NULL || fields == NULL || values == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }

    for (uint32_t index = 0; index < column_count; index++) {
        fields[index].table = alias;
        fields[index].name = table->schema.columns[index].name;
        fields[index].type = table->schema.columns[index].type;
    }

    scan->base.vtable = &SCAN_VTABLE;
    scan->base.shape.fields = fields;
    scan->base.shape.field_count = column_count;
    scan->pager = pager;
    scan->table = table;
    scan->first_key = INT64_MIN;
    scan->last_key = INT64_MAX;
    scan->values = values;
    minidb_cursor_init(&scan->cursor, pager);

    *out_scan = &scan->base;
    return MINIDB_OK;
}

void minidb_scan_set_key_range(minidb_operator_t *self, int64_t first_key, int64_t last_key) {
    scan_t *scan = (scan_t *)self;
    scan->first_key = first_key;
    scan->last_key = last_key;
}

int64_t minidb_scan_current_key(const minidb_operator_t *self) {
    return ((const scan_t *)self)->current_key;
}
