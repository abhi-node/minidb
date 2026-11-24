#include "minidb/executor.h"

#include <inttypes.h>
#include <string.h>

#include "minidb/btree.h"
#include "minidb/expression.h"
#include "minidb/operator.h"
#include "minidb/planner.h"
#include "minidb/row.h"

/* Everything a statement needs while it runs. */
typedef struct {
    minidb_storage_t *storage;
    minidb_arena_t *arena;
    minidb_error_t *error;
    uint64_t row_count;  /* rows returned or changed so far */
} execution_t;

static minidb_status_t find_table(execution_t *execution, const char *table_name,
                                  minidb_table_t **out_table) {
    *out_table = minidb_catalog_find(minidb_storage_catalog(execution->storage), table_name);
    if (*out_table == NULL) {
        return minidb_error_set(execution->error, MINIDB_ERR_UNKNOWN_TABLE,
                                "no such table '%s'", table_name);
    }
    return MINIDB_OK;
}

/* ---- Shared by INSERT and UPDATE ----------------------------------------- */

/* True if a value of `value_type` may be stored in a column of `column_type`. */
static bool is_assignable(minidb_type_t column_type, minidb_type_t value_type) {
    return column_type == value_type ||
           (column_type == MINIDB_TYPE_DECIMAL && value_type == MINIDB_TYPE_INT);
}

static minidb_status_t check_assignable(execution_t *execution, const minidb_column_t *column,
                                        minidb_type_t value_type) {
    if (is_assignable(column->type, value_type)) {
        return MINIDB_OK;
    }
    return minidb_error_set(execution->error, MINIDB_ERR_TYPE_MISMATCH,
                            "column '%s' is %s but the value is %s", column->name,
                            minidb_type_name(column->type), minidb_type_name(value_type));
}

/* Converts an assignable value to exactly the column's type. */
static minidb_value_t convert_for_column(minidb_type_t column_type, minidb_value_t value) {
    if (column_type == MINIDB_TYPE_DECIMAL && value.type == MINIDB_TYPE_INT) {
        return minidb_value_decimal((double)value.as.integer);
    }
    return value;
}

/* Serializes `values` into `out_row`, which holds MINIDB_BTREE_MAX_PAYLOAD bytes. */
static minidb_status_t encode_row(execution_t *execution, const minidb_table_t *table,
                                  const minidb_value_t *values, uint8_t *out_row,
                                  uint32_t *out_length) {
    size_t row_length = minidb_row_encoded_size(&table->schema, values);
    if (row_length > MINIDB_BTREE_MAX_PAYLOAD) {
        return minidb_error_set(execution->error, MINIDB_ERR_ROW_TOO_LARGE,
                                "row is %zu bytes; the limit is %u", row_length,
                                MINIDB_BTREE_MAX_PAYLOAD);
    }
    minidb_row_encode(&table->schema, values, out_row);
    *out_length = (uint32_t)row_length;
    return MINIDB_OK;
}

/* ---- SELECT -------------------------------------------------------------- */

static minidb_status_t execute_select(execution_t *execution, minidb_select_statement_t *select,
                                      minidb_row_callback_t on_row, void *context) {
    minidb_operator_t *plan;
    minidb_status_t status =
        minidb_plan_select(execution->storage, execution->arena, select, &plan, execution->error);
    if (status != MINIDB_OK) {
        return status;
    }

    uint32_t column_count = plan->shape.field_count;
    const char **column_names =
        minidb_arena_alloc(execution->arena, column_count * sizeof(*column_names));
    if (column_names == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    for (uint32_t column = 0; column < column_count; column++) {
        column_names[column] = plan->shape.fields[column].name;
    }

    minidb_row_t row = {.column_count = column_count, .column_names = column_names};
    status = minidb_operator_open(plan);
    while (status == MINIDB_OK &&
           (status = minidb_operator_next(plan, &row.values)) == MINIDB_OK) {
        execution->row_count++;
        if (on_row != NULL && !on_row(context, &row)) {
            status = MINIDB_ERR_ABORTED;
        }
    }
    minidb_operator_close(plan);
    return status == MINIDB_DONE ? MINIDB_OK : status;
}

/* ---- INSERT -------------------------------------------------------------- */

/*
 * Works out which table column each VALUES position feeds. Without a column
 * list the values are in schema order; with one, every column must be named
 * exactly once (there are no defaults or NULLs to fill a gap with).
 */
static minidb_status_t map_insert_columns(execution_t *execution,
                                          const minidb_insert_statement_t *insert,
                                          const minidb_table_t *table,
                                          uint32_t *out_column_of_value) {
    const minidb_schema_t *schema = &table->schema;
    if (insert->values_per_row != schema->column_count) {
        return minidb_error_set(execution->error, MINIDB_ERR_TYPE_MISMATCH,
                                "table '%s' has %u columns but %u values were given",
                                schema->table_name, schema->column_count,
                                insert->values_per_row);
    }
    if (insert->column_names == NULL) {
        for (uint32_t position = 0; position < schema->column_count; position++) {
            out_column_of_value[position] = position;
        }
        return MINIDB_OK;
    }
    if (insert->column_name_count != insert->values_per_row) {
        return minidb_error_set(execution->error, MINIDB_ERR_TYPE_MISMATCH,
                                "%u columns are listed but %u values were given",
                                insert->column_name_count, insert->values_per_row);
    }

    bool is_column_used[MINIDB_MAX_COLUMNS] = {false};
    for (uint32_t position = 0; position < insert->column_name_count; position++) {
        const char *column_name = insert->column_names[position];
        int32_t column = minidb_schema_find_column(schema, column_name);
        if (column < 0) {
            return minidb_error_set(execution->error, MINIDB_ERR_UNKNOWN_COLUMN,
                                    "table '%s' has no column '%s'", schema->table_name,
                                    column_name);
        }
        if (is_column_used[column]) {
            return minidb_error_set(execution->error, MINIDB_ERR_TYPE_MISMATCH,
                                    "column '%s' is listed twice", column_name);
        }
        is_column_used[column] = true;
        out_column_of_value[position] = (uint32_t)column;
    }
    return MINIDB_OK;
}

/* Evaluates one VALUES row into `out_values`, in schema order. */
static minidb_status_t evaluate_insert_row(execution_t *execution, const minidb_table_t *table,
                                           minidb_expr_t **row_exprs,
                                           const uint32_t *column_of_value,
                                           minidb_value_t *out_values) {
    for (uint32_t position = 0; position < table->schema.column_count; position++) {
        const minidb_column_t *column = &table->schema.columns[column_of_value[position]];
        minidb_expr_t *expr = row_exprs[position];
        minidb_value_t value;

        /* VALUES are constants: bind with no row shape, evaluate with no row. */
        minidb_status_t status = minidb_expr_bind(expr, NULL, execution->error);
        if (status == MINIDB_OK) {
            status = check_assignable(execution, column, expr->type);
        }
        if (status == MINIDB_OK) {
            status = minidb_expr_evaluate(expr, NULL, &value);
            if (status != MINIDB_OK) {
                minidb_error_set(execution->error, status, "%s", minidb_status_name(status));
            }
        }
        if (status != MINIDB_OK) {
            return status;
        }
        out_values[column_of_value[position]] = convert_for_column(column->type, value);
    }
    return MINIDB_OK;
}

static minidb_status_t insert_one_row(execution_t *execution, minidb_table_t *table,
                                      const minidb_value_t *values, int64_t *out_key) {
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t row_length = 0;
    minidb_status_t status = encode_row(execution, table, values, row, &row_length);
    if (status != MINIDB_OK) {
        return status;
    }

    int32_t key_column = table->schema.primary_key_index;
    int64_t key =
        key_column == MINIDB_NO_PRIMARY_KEY ? table->next_row_id : values[key_column].as.integer;

    status = minidb_storage_insert(execution->storage, table, key, row, row_length);
    if (status == MINIDB_ERR_DUPLICATE_KEY) {
        return minidb_error_set(execution->error, status,
                                "duplicate primary key %" PRId64 " in table '%s'", key,
                                table->schema.table_name);
    }
    *out_key = key;
    return status;
}

static minidb_status_t execute_insert(execution_t *execution, minidb_insert_statement_t *insert) {
    minidb_table_t *table;
    minidb_status_t status = find_table(execution, insert->table_name, &table);
    if (status != MINIDB_OK) {
        return status;
    }

    uint32_t column_of_value[MINIDB_MAX_COLUMNS];
    status = map_insert_columns(execution, insert, table, column_of_value);
    if (status != MINIDB_OK) {
        return status;
    }

    /* Remember what was inserted so a failure on a later row can undo it. */
    int64_t *inserted_keys =
        minidb_arena_alloc(execution->arena, insert->row_count * sizeof(*inserted_keys));
    if (inserted_keys == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    int64_t next_row_id_before = table->next_row_id;
    uint32_t inserted_count = 0;

    for (uint32_t row = 0; row < insert->row_count && status == MINIDB_OK; row++) {
        minidb_value_t values[MINIDB_MAX_COLUMNS];
        minidb_expr_t **row_exprs = insert->values + (size_t)row * insert->values_per_row;
        status = evaluate_insert_row(execution, table, row_exprs, column_of_value, values);
        if (status == MINIDB_OK) {
            status = insert_one_row(execution, table, values, &inserted_keys[inserted_count]);
        }
        if (status == MINIDB_OK) {
            inserted_count++;
        }
    }

    if (status != MINIDB_OK) {
        for (uint32_t index = 0; index < inserted_count; index++) {
            minidb_storage_delete(execution->storage, table, inserted_keys[index]);
        }
        table->next_row_id = next_row_id_before;
        minidb_storage_abort(execution->storage);
        return status;
    }
    execution->row_count = inserted_count;
    return minidb_storage_commit(execution->storage);
}

/* ---- UPDATE / DELETE ----------------------------------------------------- */

/* A change collected during the scan phase, to be applied after it. */
typedef struct {
    int64_t key;
    const uint8_t *row;   /* the new row for UPDATE (arena memory); NULL for DELETE */
    uint32_t row_length;
} pending_change_t;

typedef struct {
    pending_change_t *changes;  /* arena memory, regrown by doubling */
    size_t count;
    size_t capacity;
} pending_list_t;

static minidb_status_t pending_list_push(execution_t *execution, pending_list_t *list,
                                         pending_change_t change) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        pending_change_t *changes =
            minidb_arena_alloc(execution->arena, capacity * sizeof(*changes));
        if (changes == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        if (list->count > 0) {
            memcpy(changes, list->changes, list->count * sizeof(*changes));
        }
        list->changes = changes;
        list->capacity = capacity;
    }
    list->changes[list->count++] = change;
    return MINIDB_OK;
}

/* Returns the next row of `scan` that satisfies `where` (NULL matches all). */
static minidb_status_t next_matching_row(minidb_operator_t *scan, const minidb_expr_t *where,
                                         const minidb_value_t **out_row) {
    for (;;) {
        minidb_status_t status = minidb_operator_next(scan, out_row);
        if (status != MINIDB_OK || where == NULL) {
            return status;
        }
        minidb_value_t is_match;
        status = minidb_expr_evaluate(where, *out_row, &is_match);
        if (status != MINIDB_OK || is_match.as.boolean) {
            return status;
        }
    }
}

/* Binds each SET expression and resolves the column it assigns. */
static minidb_status_t bind_assignments(execution_t *execution, const minidb_table_t *table,
                                        const minidb_update_statement_t *update,
                                        const minidb_row_shape_t *shape,
                                        uint32_t *out_target_columns) {
    for (uint32_t index = 0; index < update->assignment_count; index++) {
        const minidb_assignment_t *assignment = &update->assignments[index];
        int32_t column = minidb_schema_find_column(&table->schema, assignment->column_name);
        if (column < 0) {
            return minidb_error_set(execution->error, MINIDB_ERR_UNKNOWN_COLUMN,
                                    "table '%s' has no column '%s'", table->schema.table_name,
                                    assignment->column_name);
        }
        if (column == table->schema.primary_key_index) {
            return minidb_error_set(execution->error, MINIDB_ERR_UNSUPPORTED,
                                    "cannot update primary key '%s'; delete and re-insert",
                                    assignment->column_name);
        }
        minidb_status_t status = minidb_expr_bind(assignment->value, shape, execution->error);
        if (status == MINIDB_OK) {
            status = check_assignable(execution, &table->schema.columns[column],
                                      assignment->value->type);
        }
        if (status != MINIDB_OK) {
            return status;
        }
        out_target_columns[index] = (uint32_t)column;
    }
    return MINIDB_OK;
}

/* Builds the updated version of `old_row` and queues it. */
static minidb_status_t queue_updated_row(execution_t *execution, const minidb_table_t *table,
                                         const minidb_update_statement_t *update,
                                         const uint32_t *target_columns, int64_t key,
                                         const minidb_value_t *old_row, pending_list_t *pending) {
    minidb_value_t new_values[MINIDB_MAX_COLUMNS];
    memcpy(new_values, old_row, table->schema.column_count * sizeof(*old_row));

    /* Every SET expression sees the row as it was before the statement. */
    for (uint32_t index = 0; index < update->assignment_count; index++) {
        uint32_t column = target_columns[index];
        minidb_value_t value;
        minidb_status_t status =
            minidb_expr_evaluate(update->assignments[index].value, old_row, &value);
        if (status != MINIDB_OK) {
            return minidb_error_set(execution->error, status, "%s", minidb_status_name(status));
        }
        new_values[column] = convert_for_column(table->schema.columns[column].type, value);
    }

    /* The scan's row is only borrowed, so the new row is serialized now. */
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t row_length = 0;
    minidb_status_t status = encode_row(execution, table, new_values, row, &row_length);
    if (status != MINIDB_OK) {
        return status;
    }
    uint8_t *saved_row = minidb_arena_alloc(execution->arena, row_length);
    if (saved_row == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    memcpy(saved_row, row, row_length);
    return pending_list_push(execution, pending, (pending_change_t){key, saved_row, row_length});
}

/*
 * Phase 2 of UPDATE and DELETE: the scan is closed, so the tree can be
 * modified freely. Queued changes cannot fail on their own account (the
 * keys exist and the rows fit); an I/O or memory error here aborts the
 * statement's log records and is reported.
 */
static minidb_status_t apply_pending_changes(execution_t *execution, minidb_table_t *table,
                                             const pending_list_t *pending) {
    minidb_status_t status = MINIDB_OK;
    for (size_t index = 0; index < pending->count && status == MINIDB_OK; index++) {
        const pending_change_t *change = &pending->changes[index];
        status = change->row != NULL
                     ? minidb_storage_update(execution->storage, table, change->key,
                                             change->row, change->row_length)
                     : minidb_storage_delete(execution->storage, table, change->key);
    }
    if (status != MINIDB_OK) {
        minidb_storage_abort(execution->storage);
        return status;
    }
    execution->row_count = pending->count;
    return minidb_storage_commit(execution->storage);
}

static minidb_status_t execute_update(execution_t *execution, minidb_update_statement_t *update) {
    minidb_table_t *table;
    minidb_operator_t *scan;
    uint32_t target_columns[MINIDB_MAX_COLUMNS];

    minidb_status_t status = find_table(execution, update->table_name, &table);
    if (status == MINIDB_OK && update->assignment_count > MINIDB_MAX_COLUMNS) {
        status = minidb_error_set(execution->error, MINIDB_ERR_UNSUPPORTED,
                                  "too many assignments");
    }
    if (status == MINIDB_OK) {
        status = minidb_plan_table_scan(execution->storage, execution->arena, table,
                                        update->where, &scan, execution->error);
    }
    if (status == MINIDB_OK) {
        status = bind_assignments(execution, table, update, &scan->shape, target_columns);
    }
    if (status != MINIDB_OK) {
        return status;
    }

    pending_list_t pending = {0};
    const minidb_value_t *row;
    status = minidb_operator_open(scan);
    while (status == MINIDB_OK &&
           (status = next_matching_row(scan, update->where, &row)) == MINIDB_OK) {
        status = queue_updated_row(execution, table, update, target_columns,
                                   minidb_scan_current_key(scan), row, &pending);
    }
    minidb_operator_close(scan);

    if (status != MINIDB_DONE) {
        return status;
    }
    return apply_pending_changes(execution, table, &pending);
}

static minidb_status_t execute_delete(execution_t *execution,
                                      minidb_delete_statement_t *delete_rows) {
    minidb_table_t *table;
    minidb_operator_t *scan;

    minidb_status_t status = find_table(execution, delete_rows->table_name, &table);
    if (status == MINIDB_OK) {
        status = minidb_plan_table_scan(execution->storage, execution->arena, table,
                                        delete_rows->where, &scan, execution->error);
    }
    if (status != MINIDB_OK) {
        return status;
    }

    pending_list_t pending = {0};
    const minidb_value_t *row;
    status = minidb_operator_open(scan);
    while (status == MINIDB_OK &&
           (status = next_matching_row(scan, delete_rows->where, &row)) == MINIDB_OK) {
        pending_change_t removal = {.key = minidb_scan_current_key(scan)};
        status = pending_list_push(execution, &pending, removal);
    }
    minidb_operator_close(scan);

    if (status != MINIDB_DONE) {
        return status;
    }
    return apply_pending_changes(execution, table, &pending);
}

/* ---- CREATE / DROP TABLE ------------------------------------------------- */

static minidb_status_t execute_create_table(execution_t *execution,
                                            const minidb_schema_t *schema) {
    minidb_table_t *table;
    minidb_status_t status = minidb_storage_create_table(execution->storage, schema, &table);
    if (status == MINIDB_ERR_TABLE_EXISTS) {
        return minidb_error_set(execution->error, status, "table '%s' already exists",
                                schema->table_name);
    }
    if (status == MINIDB_ERR_ROW_TOO_LARGE) {
        return minidb_error_set(execution->error, status,
                                "the definition of table '%s' is too large to store",
                                schema->table_name);
    }
    if (status != MINIDB_OK) {
        return status;
    }
    return minidb_storage_commit(execution->storage);
}

static minidb_status_t execute_drop_table(execution_t *execution, const char *table_name) {
    minidb_table_t *table;
    minidb_status_t status = find_table(execution, table_name, &table);
    if (status == MINIDB_OK) {
        status = minidb_storage_drop_table(execution->storage, table);
    }
    if (status != MINIDB_OK) {
        return status;
    }
    return minidb_storage_commit(execution->storage);
}

/* ---- Entry point --------------------------------------------------------- */

minidb_status_t minidb_execute(minidb_storage_t *storage, minidb_arena_t *arena,
                               minidb_statement_t *statement, minidb_row_callback_t on_row,
                               void *context, uint64_t *out_row_count, minidb_error_t *error) {
    execution_t execution = {.storage = storage, .arena = arena, .error = error};
    minidb_status_t status = MINIDB_ERR_INVALID_ARGUMENT;

    switch (statement->kind) {
        case MINIDB_STATEMENT_SELECT:
            status = execute_select(&execution, &statement->as.select, on_row, context);
            break;
        case MINIDB_STATEMENT_INSERT:
            status = execute_insert(&execution, &statement->as.insert);
            break;
        case MINIDB_STATEMENT_UPDATE:
            status = execute_update(&execution, &statement->as.update);
            break;
        case MINIDB_STATEMENT_DELETE:
            status = execute_delete(&execution, &statement->as.delete_rows);
            break;
        case MINIDB_STATEMENT_CREATE_TABLE:
            status = execute_create_table(&execution, statement->as.create_table);
            break;
        case MINIDB_STATEMENT_DROP_TABLE:
            status = execute_drop_table(&execution, statement->as.drop_table);
            break;
    }

    if (out_row_count != NULL) {
        *out_row_count = execution.row_count;
    }
    return status;
}
