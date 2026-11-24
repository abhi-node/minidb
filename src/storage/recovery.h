#ifndef MINIDB_RECOVERY_H
#define MINIDB_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "minidb/status.h"
#include "minidb/storage.h"

/*
 * Crash recovery, in two stages around opening the pager.
 *
 * Stage 1 (before the pager opens): if the log holds a complete checkpoint,
 * copy its page images into the database file. This finishes a checkpoint
 * that crashed half-way through overwriting the file.
 *
 * Stage 2 (after the pager and catalog are open): replay every committed
 * statement logged after that checkpoint.
 */

/*
 * Stage 1. *out_replay_offset is where stage 2 must start reading the log.
 */
minidb_status_t minidb_recovery_restore_checkpoint(const char *database_path,
                                                   const char *wal_path,
                                                   size_t *out_replay_offset);

/* Stage 2. Reports how many statements were replayed. */
minidb_status_t minidb_recovery_replay(minidb_storage_t *storage, const char *wal_path,
                                       size_t replay_offset, uint64_t *out_statement_count);

#endif
