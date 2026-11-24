#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include <stdint.h>

#include "minidb/catalog.h"
#include "minidb/pager.h"
#include "minidb/status.h"
#include "minidb/wal.h"

/*
 * The storage engine: tables of rows that survive crashes.
 *
 * It ties together the three on-disk pieces:
 *
 *   pager    the database file, cached as 4 KB pages
 *   B+-tree  one per table, living in those pages
 *   WAL      the log that makes changes durable before pages are written
 *
 * Every change follows the same path: it is applied to the cached pages,
 * appended to the log, and made durable when the statement commits. The
 * database file is only brought up to date by a checkpoint.
 *
 *   change -> apply to cached pages -> append to WAL -> commit (fsync policy)
 *                                                          |
 *                    checkpoint: page images -> WAL -> database file -> empty WAL
 *
 * Ownership: the storage engine owns the pager, catalog and log. Table
 * pointers it hands out belong to the catalog.
 */

typedef struct {
    minidb_sync_mode_t sync_mode;
    uint32_t group_commit_delay_ms;  /* GROUP: longest a commit waits for its fsync */
    uint32_t group_commit_bytes;     /* GROUP/OFF: flush once this much log is buffered */
    uint32_t cache_pages;            /* target size of the page cache */
    uint64_t checkpoint_wal_bytes;   /* checkpoint once the log grows past this */
} minidb_options_t;

/* GROUP commit with a 5 ms window, a 64 MB cache and a 32 MB log. */
minidb_options_t minidb_options_default(void);

typedef struct {
    minidb_wal_stats_t wal;
    minidb_pager_stats_t pager;
    uint64_t checkpoints;
    uint64_t recovered_statements;  /* statements replayed from the log at open */
} minidb_storage_stats_t;

typedef struct minidb_storage minidb_storage_t;

/*
 * Opens a database, creating it if needed. If the previous process did not
 * shut down cleanly, this is where the log is replayed.
 */
minidb_status_t minidb_storage_open(const char *path, const minidb_options_t *options,
                                    minidb_storage_t **out_storage);

/* Checkpoints and releases everything. */
minidb_status_t minidb_storage_close(minidb_storage_t *storage);

minidb_pager_t *minidb_storage_pager(const minidb_storage_t *storage);
minidb_catalog_t *minidb_storage_catalog(const minidb_storage_t *storage);

/*
 * Changes. Each one is applied and logged as part of the open statement;
 * nothing is durable until minidb_storage_commit().
 */
minidb_status_t minidb_storage_create_table(minidb_storage_t *storage,
                                            const minidb_schema_t *schema,
                                            minidb_table_t **out_table);
minidb_status_t minidb_storage_drop_table(minidb_storage_t *storage, minidb_table_t *table);
minidb_status_t minidb_storage_insert(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key, const uint8_t *row, uint32_t row_length);
minidb_status_t minidb_storage_update(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key, const uint8_t *row, uint32_t row_length);
minidb_status_t minidb_storage_delete(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key);

/*
 * Ends the open statement. Durability follows the sync mode. May run a
 * checkpoint, so the caller must not hold any page pinned.
 */
minidb_status_t minidb_storage_commit(minidb_storage_t *storage);

/*
 * Drops the open statement's log records. The caller must already have
 * reversed the statement's changes (e.g. deleted the rows it inserted).
 */
void minidb_storage_abort(minidb_storage_t *storage);

/* Makes every committed statement durable now, whatever the sync mode. */
minidb_status_t minidb_storage_sync(minidb_storage_t *storage);

/* Brings the database file up to date and empties the log. */
minidb_status_t minidb_storage_checkpoint(minidb_storage_t *storage);

minidb_storage_stats_t minidb_storage_stats(const minidb_storage_t *storage);

/*
 * Applies one logged change to the cached pages without logging it again.
 * Recovery uses this to replay the log; live changes go through it too, so
 * replay and normal operation cannot drift apart.
 */
minidb_status_t minidb_storage_apply(minidb_storage_t *storage,
                                     const minidb_wal_record_t *record);

#endif
