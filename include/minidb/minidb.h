#ifndef MINIDB_H
#define MINIDB_H

/*
 * minidb's public API: open a database file and run SQL against it.
 *
 *     minidb_t *db;
 *     minidb_open("shop.db", NULL, &db);
 *     minidb_exec(db, "CREATE TABLE items (id INT PRIMARY KEY, name TEXT)", NULL, NULL);
 *     minidb_exec(db, "INSERT INTO items VALUES (1, 'kettle')", NULL, NULL);
 *     minidb_exec(db, "SELECT * FROM items WHERE id = 1", print_row, NULL);
 *     minidb_close(db);
 *
 * A handle is single-threaded: use it from one thread at a time.
 */

#include <stdint.h>

#include "minidb/executor.h" /* minidb_row_t, minidb_row_callback_t */
#include "minidb/schema.h"
#include "minidb/status.h"
#include "minidb/storage.h"  /* minidb_options_t, minidb_storage_stats_t */

typedef struct minidb minidb_t;

/*
 * Opens the database at `path`, creating it if it does not exist and
 * recovering it if the last process crashed. The log lives next to it at
 * "<path>.wal". Pass NULL options for the defaults.
 */
minidb_status_t minidb_open(const char *path, const minidb_options_t *options,
                            minidb_t **out_db);

/* Checkpoints and closes. The handle is invalid afterwards, even on error. */
minidb_status_t minidb_close(minidb_t *db);

/*
 * Runs every statement in `sql`, in order, stopping at the first failure.
 * `on_row` (which may be NULL) is called for each row a SELECT returns.
 */
minidb_status_t minidb_exec(minidb_t *db, const char *sql, minidb_row_callback_t on_row,
                            void *context);

/* Describes the most recent failure on this handle. Never NULL. */
const char *minidb_error_message(const minidb_t *db);

/* Rows returned or changed by the last statement that ran. */
uint64_t minidb_last_row_count(const minidb_t *db);

/* Forces every committed statement to disk now, whatever the sync mode. */
minidb_status_t minidb_sync(minidb_t *db);

/* Writes all changes into the database file and empties the log. */
minidb_status_t minidb_checkpoint(minidb_t *db);

minidb_storage_stats_t minidb_stats(const minidb_t *db);

/* Schema introspection, for tools such as the CLI's .tables command. */
uint32_t minidb_table_count(const minidb_t *db);
const minidb_schema_t *minidb_table_schema(const minidb_t *db, uint32_t table_index);

#endif
