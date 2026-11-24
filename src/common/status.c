#include "minidb/status.h"

#include <stdarg.h>
#include <stdio.h>

const char *minidb_status_name(minidb_status_t status) {
    switch (status) {
        case MINIDB_OK:                   return "ok";
        case MINIDB_DONE:                 return "done";
        case MINIDB_ERR_NO_MEMORY:        return "out of memory";
        case MINIDB_ERR_IO:               return "I/O error";
        case MINIDB_ERR_CORRUPT:          return "database is corrupt";
        case MINIDB_ERR_INVALID_ARGUMENT: return "invalid argument";
        case MINIDB_ERR_SYNTAX:           return "syntax error";
        case MINIDB_ERR_UNKNOWN_TABLE:    return "unknown table";
        case MINIDB_ERR_UNKNOWN_COLUMN:   return "unknown column";
        case MINIDB_ERR_TABLE_EXISTS:     return "table already exists";
        case MINIDB_ERR_TYPE_MISMATCH:    return "type mismatch";
        case MINIDB_ERR_DUPLICATE_KEY:    return "duplicate key";
        case MINIDB_ERR_NOT_FOUND:        return "not found";
        case MINIDB_ERR_ROW_TOO_LARGE:    return "row too large";
        case MINIDB_ERR_DIVISION_BY_ZERO: return "division by zero";
        case MINIDB_ERR_UNSUPPORTED:      return "unsupported";
        case MINIDB_ERR_ABORTED:          return "aborted";
    }
    return "unknown status";
}

minidb_status_t minidb_error_set(minidb_error_t *error, minidb_status_t status,
                                 const char *format, ...) {
    if (error != NULL) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }
    return status;
}
