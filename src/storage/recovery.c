#include "recovery.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "minidb/wal.h"

/* A growable list of log records (their payloads stay in the reader). */
typedef struct {
    minidb_wal_record_t *records;
    size_t count;
    size_t capacity;
} record_list_t;

static minidb_status_t record_list_push(record_list_t *list, const minidb_wal_record_t *record) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        minidb_wal_record_t *records = realloc(list->records, capacity * sizeof(*records));
        if (records == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        list->records = records;
        list->capacity = capacity;
    }
    list->records[list->count++] = *record;
    return MINIDB_OK;
}

/* ---- Stage 1: finish an interrupted checkpoint --------------------------- */

static minidb_status_t write_page_images(const char *database_path, const record_list_t *images) {
    int file_descriptor = open(database_path, O_RDWR | O_CREAT, 0644);
    if (file_descriptor < 0) {
        return MINIDB_ERR_IO;
    }
    minidb_status_t status = MINIDB_OK;
    for (size_t index = 0; index < images->count && status == MINIDB_OK; index++) {
        const minidb_wal_record_t *image = &images->records[index];
        off_t offset = (off_t)image->object_id * MINIDB_PAGE_SIZE;
        if (pwrite(file_descriptor, image->payload, MINIDB_PAGE_SIZE, offset) !=
            (ssize_t)MINIDB_PAGE_SIZE) {
            status = MINIDB_ERR_IO;
        }
    }
    if (status == MINIDB_OK && fsync(file_descriptor) != 0) {
        status = MINIDB_ERR_IO;
    }
    close(file_descriptor);
    return status;
}

minidb_status_t minidb_recovery_restore_checkpoint(const char *database_path,
                                                   const char *wal_path,
                                                   size_t *out_replay_offset) {
    minidb_wal_reader_t reader;
    minidb_status_t status = minidb_wal_reader_open(wal_path, &reader);
    if (status != MINIDB_OK) {
        return status;
    }
    *out_replay_offset = reader.offset;

    /*
     * A checkpoint is a run of page images closed by an end marker. Track
     * the current run, and keep the images of the last run that was closed.
     * A run with no end marker is a checkpoint that died while logging: the
     * database file was never touched, so its images are simply ignored.
     */
    record_list_t current_run = {0};
    record_list_t last_complete = {0};
    minidb_wal_record_t record;

    while (status == MINIDB_OK &&
           (status = minidb_wal_reader_next(&reader, &record)) == MINIDB_OK) {
        if (record.type == MINIDB_WAL_PAGE_IMAGE) {
            status = record.payload_length == MINIDB_PAGE_SIZE
                         ? record_list_push(&current_run, &record)
                         : MINIDB_ERR_CORRUPT;
            continue;
        }
        if (record.type == MINIDB_WAL_CHECKPOINT_END) {
            if ((uint64_t)record.key != current_run.count) {
                status = MINIDB_ERR_CORRUPT;
                continue;
            }
            record_list_t previous = last_complete;
            last_complete = current_run;
            current_run = previous;
            *out_replay_offset = reader.offset;
        }
        current_run.count = 0;
    }
    if (status == MINIDB_DONE) {
        status = MINIDB_OK;
    }

    if (status == MINIDB_OK && last_complete.count > 0) {
        status = write_page_images(database_path, &last_complete);
    }

    free(current_run.records);
    free(last_complete.records);
    minidb_wal_reader_close(&reader);
    return status;
}

/* ---- Stage 2: replay committed statements -------------------------------- */

minidb_status_t minidb_recovery_replay(minidb_storage_t *storage, const char *wal_path,
                                       size_t replay_offset, uint64_t *out_statement_count) {
    *out_statement_count = 0;

    minidb_wal_reader_t reader;
    minidb_status_t status = minidb_wal_reader_open(wal_path, &reader);
    if (status != MINIDB_OK) {
        return status;
    }
    reader.offset = replay_offset;

    /* Records are held back until their statement's last record shows up,
     * so a statement cut short by the crash is never half-applied. */
    record_list_t statement = {0};
    minidb_wal_record_t record;

    while (status == MINIDB_OK &&
           (status = minidb_wal_reader_next(&reader, &record)) == MINIDB_OK) {
        if (record.type == MINIDB_WAL_PAGE_IMAGE || record.type == MINIDB_WAL_CHECKPOINT_END) {
            continue;  /* debris of a checkpoint that never completed */
        }
        status = record_list_push(&statement, &record);
        if (status != MINIDB_OK || !record.ends_statement) {
            continue;
        }
        for (size_t index = 0; index < statement.count && status == MINIDB_OK; index++) {
            status = minidb_storage_apply(storage, &statement.records[index]);
        }
        /* The log said this change succeeded once; it must succeed again. */
        if (status != MINIDB_OK && status != MINIDB_ERR_NO_MEMORY && status != MINIDB_ERR_IO) {
            status = MINIDB_ERR_CORRUPT;
        }
        statement.count = 0;
        (*out_statement_count)++;
    }
    if (status == MINIDB_DONE) {
        status = MINIDB_OK;
    }

    free(statement.records);
    minidb_wal_reader_close(&reader);
    return status;
}
