#ifndef MINIDB_EXECUTOR_H
#define MINIDB_EXECUTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "minidb/arena.h"
#include "minidb/ast.h"
#include "minidb/status.h"
#include "minidb/storage.h"
#include "minidb/value.h"

/*
 * Runs one parsed statement against the storage engine.
 *
 *   SELECT            plans an operator tree and hands each row to a callback
 *   INSERT            evaluates and type-checks the values, then inserts
 *   UPDATE / DELETE   first collect the matching rows, then change them, so
 *                     a statement never scans rows it has itself modified
 *   CREATE / DROP     go straight to the catalog
 *
 * Every statement is atomic and commits on success. A statement that fails
 * half-way (say, a duplicate key in the third row of an INSERT) undoes what
 * it did and leaves nothing in the log.
 */

/* One result row, as passed to a row callback. */
typedef struct {
    uint32_t column_count;
    const char *const *column_names;
    const minidb_value_t *values;  /* borrowed: valid only during the callback */
} minidb_row_t;

/* Called once per result row. Return false to stop the query early. */
typedef bool (*minidb_row_callback_t)(void *context, const minidb_row_t *row);

/*
 * Executes `statement`. `on_row` may be NULL to discard result rows.
 * *out_row_count receives the number of rows returned or changed.
 *
 * `arena` supplies the statement's working memory; the caller resets it
 * afterwards.
 */
minidb_status_t minidb_execute(minidb_storage_t *storage, minidb_arena_t *arena,
                               minidb_statement_t *statement, minidb_row_callback_t on_row,
                               void *context, uint64_t *out_row_count, minidb_error_t *error);

#endif
