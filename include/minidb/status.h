#ifndef MINIDB_STATUS_H
#define MINIDB_STATUS_H

/*
 * Status codes shared by every module.
 *
 * Every fallible function returns a minidb_status_t. MINIDB_DONE is the one
 * non-error, non-OK value: iterators return it when they run out of rows.
 */

typedef enum {
    MINIDB_OK = 0,
    MINIDB_DONE,                 /* iterator exhausted; not an error */
    MINIDB_ERR_NO_MEMORY,
    MINIDB_ERR_IO,               /* open/read/write/fsync failed */
    MINIDB_ERR_CORRUPT,          /* on-disk bytes are not what we wrote */
    MINIDB_ERR_INVALID_ARGUMENT,
    MINIDB_ERR_SYNTAX,           /* SQL could not be parsed */
    MINIDB_ERR_UNKNOWN_TABLE,
    MINIDB_ERR_UNKNOWN_COLUMN,
    MINIDB_ERR_TABLE_EXISTS,
    MINIDB_ERR_TYPE_MISMATCH,    /* rejected by the type checker */
    MINIDB_ERR_DUPLICATE_KEY,
    MINIDB_ERR_NOT_FOUND,
    MINIDB_ERR_ROW_TOO_LARGE,
    MINIDB_ERR_DIVISION_BY_ZERO,
    MINIDB_ERR_UNSUPPORTED,
    MINIDB_ERR_ABORTED           /* a row callback asked to stop */
} minidb_status_t;

/* Human-readable message for the most recent failure of one database handle. */
typedef struct {
    char message[256];
} minidb_error_t;

/* Static name of a status code, e.g. "duplicate key". Never NULL. */
const char *minidb_status_name(minidb_status_t status);

/*
 * Formats a message into `error` (which may be NULL) and returns `status`,
 * so a failure can be reported and propagated in one statement:
 *
 *     return minidb_error_set(error, MINIDB_ERR_SYNTAX, "expected ')'");
 */
minidb_status_t minidb_error_set(minidb_error_t *error, minidb_status_t status,
                                 const char *format, ...);

#endif
