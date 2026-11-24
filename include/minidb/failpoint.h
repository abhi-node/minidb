#ifndef MINIDB_FAILPOINT_H
#define MINIDB_FAILPOINT_H

/*
 * Crash injection for recovery tests.
 *
 * A test arms one failpoint in a child process; when execution reaches it
 * the process exits on the spot, without flushing or closing anything, as if
 * it had been killed there. The parent then reopens the database and checks
 * what recovery made of the wreckage. Unarmed failpoints cost one compare.
 */

typedef enum {
    MINIDB_FAILPOINT_NONE = 0,
    MINIDB_FAILPOINT_CHECKPOINT_HALF_LOGGED,   /* some page images logged, no end marker */
    MINIDB_FAILPOINT_CHECKPOINT_LOGGED,        /* images durable, database file untouched */
    MINIDB_FAILPOINT_CHECKPOINT_HALF_WRITTEN,  /* database file partly overwritten */
    MINIDB_FAILPOINT_CHECKPOINT_WRITTEN        /* database file complete, log not yet emptied */
} minidb_failpoint_t;

/* Exit code of a process that died at a failpoint. */
#define MINIDB_FAILPOINT_EXIT_CODE 77

void minidb_failpoint_arm(minidb_failpoint_t failpoint);

/* Kills the process if `failpoint` is the armed one. */
void minidb_failpoint_hit(minidb_failpoint_t failpoint);

#endif
