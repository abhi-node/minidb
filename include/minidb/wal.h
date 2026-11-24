#ifndef MINIDB_WAL_H
#define MINIDB_WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "minidb/pager.h"
#include "minidb/status.h"

/*
 * Write-ahead log: an append-only file of checksummed records.
 *
 * Every change is logged as a small logical record ("insert this row into
 * table 3") and the statement is committed by flagging its last record. The
 * database file itself is only rewritten at a checkpoint, so after a crash
 * the log is replayed on top of the last checkpoint to rebuild the rest.
 *
 *   file:  [ header ][ record ][ record ] ...
 *   record: checksum | payload length | type | flags | object id | key | payload
 *
 * Commits are buffered and made durable according to the sync mode:
 *
 *   FULL   write + fsync on every commit. Nothing is ever lost.
 *   GROUP  commits share an fsync: the buffer is flushed once the oldest
 *          waiting commit is `group_commit_delay_ms` old or the buffer holds
 *          `group_commit_bytes`. A crash can lose that last window.
 *   OFF    written when the buffer fills, fsynced only by checkpoints.
 *
 * Records of a statement that has not committed never reach the file, so a
 * statement can be aborted by dropping them from the buffer.
 *
 * Ownership: the log owns its buffer and file descriptor. Appended payloads
 * are copied; the caller keeps ownership of what it passed in.
 */

typedef enum {
    MINIDB_SYNC_FULL = 0,
    MINIDB_SYNC_GROUP,
    MINIDB_SYNC_OFF
} minidb_sync_mode_t;

typedef struct {
    minidb_sync_mode_t sync_mode;
    uint32_t group_commit_delay_ms;  /* GROUP: longest a commit waits for its fsync */
    uint32_t group_commit_bytes;     /* GROUP/OFF: flush once this much is buffered */
} minidb_wal_options_t;

typedef enum {
    MINIDB_WAL_INSERT = 1,      /* object id = table, key = row key, payload = row */
    MINIDB_WAL_UPDATE,          /* same fields as INSERT; replaces the row */
    MINIDB_WAL_DELETE,          /* object id = table, key = row key */
    MINIDB_WAL_CREATE_TABLE,    /* payload = encoded schema */
    MINIDB_WAL_DROP_TABLE,      /* object id = table */
    MINIDB_WAL_PAGE_IMAGE,      /* object id = page id, payload = the 4 KB page */
    MINIDB_WAL_CHECKPOINT_END   /* key = number of page images just before it */
} minidb_wal_record_type_t;

typedef struct {
    minidb_wal_record_type_t type;
    bool ends_statement;     /* set by commit; meaningful when reading back */
    uint32_t object_id;
    int64_t key;
    const uint8_t *payload;  /* borrowed */
    uint32_t payload_length;
} minidb_wal_record_t;

typedef struct {
    uint64_t commits;
    uint64_t fsyncs;
    uint64_t write_calls;
    uint64_t bytes_written;
} minidb_wal_stats_t;

typedef struct minidb_wal minidb_wal_t;

/*
 * Opens (or creates) the log for appending. Anything after the last
 * committed record -- a torn or uncommitted tail left by a crash -- is cut
 * off, so new records always follow a clean prefix.
 */
minidb_status_t minidb_wal_open(const char *path, const minidb_wal_options_t *options,
                                minidb_wal_t **out_wal);

/* Closes the file. Buffered records that were never synced are dropped. */
void minidb_wal_close(minidb_wal_t *wal);

/* Buffers one record as part of the open statement. */
minidb_status_t minidb_wal_append(minidb_wal_t *wal, const minidb_wal_record_t *record);

/* Ends the open statement and applies the sync mode. A no-op if it is empty. */
minidb_status_t minidb_wal_commit(minidb_wal_t *wal);

/* Drops the open statement's records. */
void minidb_wal_abort(minidb_wal_t *wal);

/* Writes and fsyncs every committed record now, whatever the sync mode. */
minidb_status_t minidb_wal_sync(minidb_wal_t *wal);

/* Checkpoint: logs a full page image. No statement may be open. */
minidb_status_t minidb_wal_append_page_image(minidb_wal_t *wal, minidb_page_id_t page_id,
                                             const uint8_t *page_data);

/* Checkpoint: marks the preceding `page_count` images complete and fsyncs. */
minidb_status_t minidb_wal_finish_checkpoint(minidb_wal_t *wal, uint32_t page_count);

/* Empties the log once a checkpoint has made its contents redundant. */
minidb_status_t minidb_wal_truncate(minidb_wal_t *wal);

/* Bytes of records in the log, buffered or written. */
uint64_t minidb_wal_size(const minidb_wal_t *wal);

minidb_wal_stats_t minidb_wal_stats(const minidb_wal_t *wal);

/* ---- Reading the log back (recovery) ------------------------------------- */

typedef struct {
    uint8_t *bytes;           /* the whole log file, owned by the reader */
    size_t length;
    size_t offset;            /* next record to return */
    size_t committed_length;  /* end of the last record that ended a statement */
} minidb_wal_reader_t;

/* Loads the log at `path`. A missing file reads as an empty log. */
minidb_status_t minidb_wal_reader_open(const char *path, minidb_wal_reader_t *reader);

/*
 * Returns the next record, whose payload points into the reader's memory.
 * MINIDB_DONE at the end of the log or at the first record that fails its
 * checksum (a torn write).
 */
minidb_status_t minidb_wal_reader_next(minidb_wal_reader_t *reader,
                                       minidb_wal_record_t *out_record);

void minidb_wal_reader_close(minidb_wal_reader_t *reader);

#endif
