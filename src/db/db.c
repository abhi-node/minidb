#include "minidb/minidb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minidb/arena.h"
#include "minidb/parser.h"

struct minidb {
    minidb_storage_t *storage;
    minidb_arena_t statement_arena;  /* reset after every statement */
    minidb_error_t error;
    uint64_t last_row_count;
};

minidb_status_t minidb_open(const char *path, const minidb_options_t *options,
                            minidb_t **out_db) {
    *out_db = NULL;
    minidb_t *db = calloc(1, sizeof(*db));
    if (db == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    minidb_status_t status = minidb_storage_open(path, options, &db->storage);
    if (status != MINIDB_OK) {
        free(db);
        return status;
    }
    minidb_arena_init(&db->statement_arena);
    *out_db = db;
    return MINIDB_OK;
}

minidb_status_t minidb_close(minidb_t *db) {
    if (db == NULL) {
        return MINIDB_OK;
    }
    minidb_status_t status = minidb_storage_close(db->storage);
    minidb_arena_destroy(&db->statement_arena);
    free(db);
    return status;
}

minidb_status_t minidb_exec(minidb_t *db, const char *sql, minidb_row_callback_t on_row,
                            void *context) {
    size_t remaining = strlen(sql);
    minidb_status_t status = MINIDB_OK;
    db->error.message[0] = '\0';

    while (status == MINIDB_OK && remaining > 0) {
        minidb_statement_t *statement;
        size_t consumed;
        status = minidb_parse_statement(sql, remaining, &db->statement_arena, &statement,
                                        &consumed, &db->error);
        if (status == MINIDB_OK && statement != NULL) {
            status = minidb_execute(db->storage, &db->statement_arena, statement, on_row,
                                    context, &db->last_row_count, &db->error);
        }
        minidb_arena_reset(&db->statement_arena);
        sql += consumed;
        remaining -= consumed;
    }

    /* Low-level failures (I/O, memory) carry no message of their own. */
    if (status != MINIDB_OK && db->error.message[0] == '\0') {
        minidb_error_set(&db->error, status, "%s", minidb_status_name(status));
    }
    return status;
}

const char *minidb_error_message(const minidb_t *db) {
    return db->error.message;
}

uint64_t minidb_last_row_count(const minidb_t *db) {
    return db->last_row_count;
}

minidb_status_t minidb_sync(minidb_t *db) {
    return minidb_storage_sync(db->storage);
}

minidb_status_t minidb_checkpoint(minidb_t *db) {
    return minidb_storage_checkpoint(db->storage);
}

minidb_storage_stats_t minidb_stats(const minidb_t *db) {
    return minidb_storage_stats(db->storage);
}

uint32_t minidb_table_count(const minidb_t *db) {
    return minidb_catalog_table_count(minidb_storage_catalog(db->storage));
}

const minidb_schema_t *minidb_table_schema(const minidb_t *db, uint32_t table_index) {
    return &minidb_catalog_table_at(minidb_storage_catalog(db->storage), table_index)->schema;
}
