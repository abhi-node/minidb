#include "minidb/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minidb/btree.h"
#include "minidb/failpoint.h"
#include "recovery.h"

struct minidb_storage {
    minidb_options_t options;
    minidb_pager_t *pager;
    minidb_catalog_t *catalog;
    minidb_wal_t *wal;
    uint64_t checkpoint_count;
    uint64_t recovered_statements;
};

minidb_options_t minidb_options_default(void) {
    minidb_options_t options = {
        .sync_mode = MINIDB_SYNC_GROUP,
        .group_commit_delay_ms = 5,
        .group_commit_bytes = 256 * 1024,
        .cache_pages = 8192,
        .checkpoint_wal_bytes = 32ull * 1024 * 1024,
    };
    return options;
}

/* ---- Applying changes ---------------------------------------------------- */

static minidb_status_t apply_create_table(minidb_storage_t *storage,
                                          const minidb_wal_record_t *record) {
    minidb_schema_t schema;
    minidb_status_t status =
        minidb_schema_decode(record->payload, record->payload_length, &schema);
    if (status != MINIDB_OK) {
        return status;
    }
    minidb_table_t *table;
    return minidb_catalog_create_table(storage->catalog, &schema, &table);
}

minidb_status_t minidb_storage_apply(minidb_storage_t *storage,
                                     const minidb_wal_record_t *record) {
    if (record->type == MINIDB_WAL_CREATE_TABLE) {
        return apply_create_table(storage, record);
    }

    minidb_table_t *table = minidb_catalog_find_by_id(storage->catalog, record->object_id);
    if (table == NULL) {
        return MINIDB_ERR_UNKNOWN_TABLE;
    }

    switch (record->type) {
        case MINIDB_WAL_INSERT: {
            minidb_status_t status =
                minidb_btree_insert(storage->pager, table->root_page, record->key,
                                    record->payload, record->payload_length);
            if (status == MINIDB_OK && record->key >= table->next_row_id) {
                table->next_row_id = record->key + 1;
            }
            return status;
        }
        case MINIDB_WAL_UPDATE:
            return minidb_btree_update(storage->pager, table->root_page, record->key,
                                       record->payload, record->payload_length);
        case MINIDB_WAL_DELETE:
            return minidb_btree_delete(storage->pager, table->root_page, record->key);
        case MINIDB_WAL_DROP_TABLE:
            return minidb_catalog_drop_table(storage->catalog, table);
        default:
            return MINIDB_ERR_CORRUPT;
    }
}

/* Applies a change and, if that worked, appends it to the open statement. */
static minidb_status_t apply_and_log(minidb_storage_t *storage,
                                     const minidb_wal_record_t *record) {
    minidb_status_t status = minidb_storage_apply(storage, record);
    if (status != MINIDB_OK) {
        return status;
    }
    return minidb_wal_append(storage->wal, record);
}

minidb_status_t minidb_storage_create_table(minidb_storage_t *storage,
                                            const minidb_schema_t *schema,
                                            minidb_table_t **out_table) {
    *out_table = NULL;

    uint8_t encoded_schema[MINIDB_BTREE_MAX_PAYLOAD];
    size_t encoded_size = minidb_schema_encoded_size(schema);
    if (encoded_size > sizeof(encoded_schema)) {
        return MINIDB_ERR_ROW_TOO_LARGE;
    }
    minidb_schema_encode(schema, encoded_schema);

    minidb_wal_record_t record = {
        .type = MINIDB_WAL_CREATE_TABLE,
        .payload = encoded_schema,
        .payload_length = (uint32_t)encoded_size,
    };
    minidb_status_t status = apply_and_log(storage, &record);
    if (status == MINIDB_OK) {
        *out_table = minidb_catalog_find(storage->catalog, schema->table_name);
    }
    return status;
}

minidb_status_t minidb_storage_drop_table(minidb_storage_t *storage, minidb_table_t *table) {
    minidb_wal_record_t record = {.type = MINIDB_WAL_DROP_TABLE, .object_id = table->id};
    return apply_and_log(storage, &record);
}

minidb_status_t minidb_storage_insert(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key, const uint8_t *row, uint32_t row_length) {
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_INSERT,
        .object_id = table->id,
        .key = key,
        .payload = row,
        .payload_length = row_length,
    };
    return apply_and_log(storage, &record);
}

minidb_status_t minidb_storage_update(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key, const uint8_t *row, uint32_t row_length) {
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_UPDATE,
        .object_id = table->id,
        .key = key,
        .payload = row,
        .payload_length = row_length,
    };
    return apply_and_log(storage, &record);
}

minidb_status_t minidb_storage_delete(minidb_storage_t *storage, minidb_table_t *table,
                                      int64_t key) {
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_DELETE,
        .object_id = table->id,
        .key = key,
    };
    return apply_and_log(storage, &record);
}

/* ---- Statement boundaries ------------------------------------------------ */

static bool checkpoint_is_due(const minidb_storage_t *storage) {
    /* Dirty pages cannot be evicted, so do not let them crowd out the cache. */
    uint32_t dirty_page_limit = storage->options.cache_pages / 2;
    return minidb_wal_size(storage->wal) >= storage->options.checkpoint_wal_bytes ||
           minidb_pager_dirty_count(storage->pager) >= dirty_page_limit;
}

minidb_status_t minidb_storage_commit(minidb_storage_t *storage) {
    minidb_status_t status = minidb_wal_commit(storage->wal);
    if (status == MINIDB_OK && checkpoint_is_due(storage)) {
        status = minidb_storage_checkpoint(storage);
    }
    return status;
}

void minidb_storage_abort(minidb_storage_t *storage) {
    minidb_wal_abort(storage->wal);
}

minidb_status_t minidb_storage_sync(minidb_storage_t *storage) {
    return minidb_wal_sync(storage->wal);
}

/* ---- Checkpoint ---------------------------------------------------------- */

typedef struct {
    minidb_wal_t *wal;
    uint32_t logged_count;
    uint32_t halfway_count;
} image_logger_t;

static minidb_status_t log_page_image(void *context, minidb_page_id_t page_id,
                                      const uint8_t *data) {
    image_logger_t *logger = context;
    if (logger->logged_count == logger->halfway_count && logger->logged_count > 0) {
        minidb_failpoint_hit(MINIDB_FAILPOINT_CHECKPOINT_HALF_LOGGED);
    }
    logger->logged_count++;
    return minidb_wal_append_page_image(logger->wal, page_id, data);
}

/*
 * Overwriting pages in place is not atomic: a crash half-way would leave a
 * mix of old and new pages that no log replay could repair. So the new page
 * images go to the log first. Once they are durable there, recovery can
 * always finish the job, and only then is the database file touched.
 *
 *   1. append an image of every dirty page to the log, then an end marker;
 *      fsync the log
 *   2. write the dirty pages into the database file; fsync it
 *   3. empty the log
 */
minidb_status_t minidb_storage_checkpoint(minidb_storage_t *storage) {
    uint32_t dirty_count = minidb_pager_dirty_count(storage->pager);
    if (dirty_count == 0 && minidb_wal_size(storage->wal) == 0) {
        return MINIDB_OK;
    }

    image_logger_t logger = {storage->wal, 0, dirty_count / 2};
    minidb_status_t status = minidb_pager_visit_dirty(storage->pager, log_page_image, &logger);
    if (status == MINIDB_OK) {
        status = minidb_wal_finish_checkpoint(storage->wal, logger.logged_count);
    }
    if (status != MINIDB_OK) {
        return status;
    }
    minidb_failpoint_hit(MINIDB_FAILPOINT_CHECKPOINT_LOGGED);

    status = minidb_pager_write_dirty(storage->pager);
    if (status != MINIDB_OK) {
        return status;
    }
    minidb_failpoint_hit(MINIDB_FAILPOINT_CHECKPOINT_WRITTEN);

    storage->checkpoint_count++;
    return minidb_wal_truncate(storage->wal);
}

/* ---- Open / close -------------------------------------------------------- */

minidb_status_t minidb_storage_open(const char *path, const minidb_options_t *options,
                                    minidb_storage_t **out_storage) {
    *out_storage = NULL;

    size_t wal_path_size = strlen(path) + sizeof(".wal");
    char *wal_path = malloc(wal_path_size);
    minidb_storage_t *storage = calloc(1, sizeof(*storage));
    if (wal_path == NULL || storage == NULL) {
        free(wal_path);
        free(storage);
        return MINIDB_ERR_NO_MEMORY;
    }
    snprintf(wal_path, wal_path_size, "%s.wal", path);
    storage->options = options != NULL ? *options : minidb_options_default();

    minidb_wal_options_t wal_options = {
        .sync_mode = storage->options.sync_mode,
        .group_commit_delay_ms = storage->options.group_commit_delay_ms,
        .group_commit_bytes = storage->options.group_commit_bytes,
    };

    /* Recovery stage 1 must run before the pager reads the file header. */
    size_t replay_offset = 0;
    minidb_status_t status =
        minidb_recovery_restore_checkpoint(path, wal_path, &replay_offset);
    if (status == MINIDB_OK) {
        status = minidb_pager_open(path, storage->options.cache_pages, &storage->pager);
    }
    if (status == MINIDB_OK) {
        status = minidb_catalog_open(storage->pager, &storage->catalog);
    }
    if (status == MINIDB_OK) {
        status = minidb_recovery_replay(storage, wal_path, replay_offset,
                                        &storage->recovered_statements);
    }
    if (status == MINIDB_OK) {
        status = minidb_wal_open(wal_path, &wal_options, &storage->wal);
    }
    /* Fold whatever was replayed into the database file and start with an
     * empty log. On a clean open this does nothing. */
    if (status == MINIDB_OK) {
        status = minidb_storage_checkpoint(storage);
    }
    free(wal_path);

    if (status != MINIDB_OK) {
        minidb_wal_close(storage->wal);
        minidb_catalog_close(storage->catalog);
        minidb_pager_close(storage->pager);
        free(storage);
        return status;
    }
    *out_storage = storage;
    return MINIDB_OK;
}

minidb_status_t minidb_storage_close(minidb_storage_t *storage) {
    if (storage == NULL) {
        return MINIDB_OK;
    }
    minidb_wal_abort(storage->wal);
    minidb_status_t status = minidb_storage_checkpoint(storage);

    minidb_wal_close(storage->wal);
    minidb_catalog_close(storage->catalog);
    minidb_pager_close(storage->pager);
    free(storage);
    return status;
}

minidb_pager_t *minidb_storage_pager(const minidb_storage_t *storage) {
    return storage->pager;
}

minidb_catalog_t *minidb_storage_catalog(const minidb_storage_t *storage) {
    return storage->catalog;
}

minidb_storage_stats_t minidb_storage_stats(const minidb_storage_t *storage) {
    minidb_storage_stats_t stats = {
        .wal = minidb_wal_stats(storage->wal),
        .pager = minidb_pager_stats(storage->pager),
        .checkpoints = storage->checkpoint_count,
        .recovered_statements = storage->recovered_statements,
    };
    return stats;
}
