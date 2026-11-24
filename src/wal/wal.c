#include "minidb/wal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "minidb/checksum.h"

/* ---- On-disk format ------------------------------------------------------ */

static const uint8_t WAL_MAGIC[8] = {'M', 'D', 'B', 'W', 'A', 'L', 0, 1};
#define WAL_HEADER_SIZE 16u  /* magic + page size + reserved */

/* Record header: every field after the checksum is covered by the checksum. */
#define RECORD_CHECKSUM_OFFSET       0
#define RECORD_PAYLOAD_LENGTH_OFFSET 4
#define RECORD_TYPE_OFFSET           8
#define RECORD_FLAGS_OFFSET          9
#define RECORD_OBJECT_ID_OFFSET      12
#define RECORD_KEY_OFFSET            16
#define RECORD_HEADER_SIZE           24u

#define RECORD_FLAG_ENDS_STATEMENT 0x01

#define RECORD_MAX_PAYLOAD MINIDB_PAGE_SIZE

#define INITIAL_BUFFER_CAPACITY (1u << 20)
#define NO_RECORD SIZE_MAX

struct minidb_wal {
    int file_descriptor;
    uint64_t file_size;  /* bytes written to the file so far */
    minidb_wal_options_t options;

    /*
     * buffer[0 .. committed_length)            whole statements, safe to write
     * buffer[committed_length .. used_length)  the open statement
     */
    uint8_t *buffer;
    size_t buffer_capacity;
    size_t used_length;
    size_t committed_length;
    size_t unsealed_record;  /* offset of the newest record, whose checksum is
                                not computed yet, or NO_RECORD */

    uint32_t unsynced_commits;         /* commits not yet covered by an fsync */
    uint64_t oldest_unsynced_commit_ns;
    bool file_needs_fsync;             /* written bytes not yet fsynced */

    minidb_wal_stats_t stats;
};

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32(uint8_t *bytes, uint32_t value) { memcpy(bytes, &value, sizeof(value)); }

static uint64_t monotonic_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint32_t record_checksum(const uint8_t *record, uint32_t payload_length) {
    return minidb_crc32(record + RECORD_PAYLOAD_LENGTH_OFFSET,
                        RECORD_HEADER_SIZE - RECORD_PAYLOAD_LENGTH_OFFSET + payload_length);
}

/* ---- Open / close -------------------------------------------------------- */

static minidb_status_t write_file_header(int file_descriptor) {
    uint8_t header[WAL_HEADER_SIZE] = {0};
    memcpy(header, WAL_MAGIC, sizeof(WAL_MAGIC));
    write_u32(header + sizeof(WAL_MAGIC), MINIDB_PAGE_SIZE);
    if (ftruncate(file_descriptor, 0) != 0 ||
        pwrite(file_descriptor, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
        fsync(file_descriptor) != 0) {
        return MINIDB_ERR_IO;
    }
    return MINIDB_OK;
}

/* Finds where the committed records end, to cut off any crash debris. */
static minidb_status_t committed_file_size(const char *path, uint64_t *out_size) {
    minidb_wal_reader_t reader;
    minidb_status_t status = minidb_wal_reader_open(path, &reader);
    if (status != MINIDB_OK) {
        return status;
    }
    minidb_wal_record_t record;
    while ((status = minidb_wal_reader_next(&reader, &record)) == MINIDB_OK) {
    }
    *out_size = reader.committed_length;
    minidb_wal_reader_close(&reader);
    return status == MINIDB_DONE ? MINIDB_OK : status;
}

minidb_status_t minidb_wal_open(const char *path, const minidb_wal_options_t *options,
                                minidb_wal_t **out_wal) {
    *out_wal = NULL;

    uint64_t committed_size;
    minidb_status_t status = committed_file_size(path, &committed_size);
    if (status != MINIDB_OK) {
        return status;
    }

    minidb_wal_t *wal = calloc(1, sizeof(*wal));
    if (wal == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    wal->options = *options;
    wal->unsealed_record = NO_RECORD;
    wal->buffer_capacity = INITIAL_BUFFER_CAPACITY;
    wal->buffer = malloc(wal->buffer_capacity);
    wal->file_descriptor = open(path, O_RDWR | O_CREAT, 0644);

    if (wal->buffer == NULL) {
        status = MINIDB_ERR_NO_MEMORY;
    } else if (wal->file_descriptor < 0) {
        status = MINIDB_ERR_IO;
    } else if (committed_size < WAL_HEADER_SIZE) {
        /* New, empty or unrecognizable: start a fresh log. */
        status = write_file_header(wal->file_descriptor);
        wal->file_size = WAL_HEADER_SIZE;
    } else {
        if (ftruncate(wal->file_descriptor, (off_t)committed_size) != 0) {
            status = MINIDB_ERR_IO;
        }
        wal->file_size = committed_size;
    }

    if (status != MINIDB_OK) {
        minidb_wal_close(wal);
        return status;
    }
    *out_wal = wal;
    return MINIDB_OK;
}

void minidb_wal_close(minidb_wal_t *wal) {
    if (wal == NULL) {
        return;
    }
    if (wal->file_descriptor >= 0) {
        close(wal->file_descriptor);
    }
    free(wal->buffer);
    free(wal);
}

/* ---- Buffer management --------------------------------------------------- */

/* Computes the checksum of the newest record, once its flags are final. */
static void seal_newest_record(minidb_wal_t *wal) {
    if (wal->unsealed_record == NO_RECORD) {
        return;
    }
    uint8_t *record = wal->buffer + wal->unsealed_record;
    uint32_t payload_length = read_u32(record + RECORD_PAYLOAD_LENGTH_OFFSET);
    write_u32(record + RECORD_CHECKSUM_OFFSET, record_checksum(record, payload_length));
    wal->unsealed_record = NO_RECORD;
}

/* Writes the committed part of the buffer to the file (without fsync). */
static minidb_status_t write_committed(minidb_wal_t *wal) {
    size_t written = 0;
    while (written < wal->committed_length) {
        ssize_t result = pwrite(wal->file_descriptor, wal->buffer + written,
                                wal->committed_length - written, (off_t)wal->file_size);
        if (result <= 0) {
            return MINIDB_ERR_IO;
        }
        written += (size_t)result;
        wal->file_size += (uint64_t)result;
        wal->stats.write_calls++;
        wal->stats.bytes_written += (uint64_t)result;
        wal->file_needs_fsync = true;
    }

    /* Slide the open statement, if any, to the front of the buffer. */
    size_t open_length = wal->used_length - wal->committed_length;
    memmove(wal->buffer, wal->buffer + wal->committed_length, open_length);
    if (wal->unsealed_record != NO_RECORD) {
        wal->unsealed_record -= wal->committed_length;
    }
    wal->used_length = open_length;
    wal->committed_length = 0;
    return MINIDB_OK;
}

/* Makes room for `needed` more bytes, writing or growing the buffer. */
static minidb_status_t reserve_buffer(minidb_wal_t *wal, size_t needed) {
    if (wal->buffer_capacity - wal->used_length >= needed) {
        return MINIDB_OK;
    }
    if (wal->committed_length > 0) {
        minidb_status_t status = write_committed(wal);
        if (status != MINIDB_OK) {
            return status;
        }
    }
    /* Still short: one open statement is larger than the buffer. Grow it,
     * because an uncommitted statement must not reach the file. */
    size_t capacity = wal->buffer_capacity;
    while (capacity - wal->used_length < needed) {
        capacity *= 2;
    }
    if (capacity != wal->buffer_capacity) {
        uint8_t *buffer = realloc(wal->buffer, capacity);
        if (buffer == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        wal->buffer = buffer;
        wal->buffer_capacity = capacity;
    }
    return MINIDB_OK;
}

static minidb_status_t fsync_file(minidb_wal_t *wal) {
    if (wal->file_needs_fsync) {
        if (fdatasync(wal->file_descriptor) != 0) {
            return MINIDB_ERR_IO;
        }
        wal->stats.fsyncs++;
        wal->file_needs_fsync = false;
    }
    wal->unsynced_commits = 0;
    return MINIDB_OK;
}

/* ---- Appending ----------------------------------------------------------- */

minidb_status_t minidb_wal_append(minidb_wal_t *wal, const minidb_wal_record_t *record) {
    if (record->payload_length > RECORD_MAX_PAYLOAD) {
        return MINIDB_ERR_INVALID_ARGUMENT;
    }
    seal_newest_record(wal);

    size_t record_size = RECORD_HEADER_SIZE + record->payload_length;
    minidb_status_t status = reserve_buffer(wal, record_size);
    if (status != MINIDB_OK) {
        return status;
    }

    uint8_t *bytes = wal->buffer + wal->used_length;
    memset(bytes, 0, RECORD_HEADER_SIZE);
    write_u32(bytes + RECORD_PAYLOAD_LENGTH_OFFSET, record->payload_length);
    bytes[RECORD_TYPE_OFFSET] = (uint8_t)record->type;
    write_u32(bytes + RECORD_OBJECT_ID_OFFSET, record->object_id);
    memcpy(bytes + RECORD_KEY_OFFSET, &record->key, sizeof(record->key));
    if (record->payload_length > 0) {
        memcpy(bytes + RECORD_HEADER_SIZE, record->payload, record->payload_length);
    }

    wal->unsealed_record = wal->used_length;
    wal->used_length += record_size;
    return MINIDB_OK;
}

/* Flags the newest record as the end of its statement and closes it. */
static void end_statement(minidb_wal_t *wal) {
    wal->buffer[wal->unsealed_record + RECORD_FLAGS_OFFSET] |= RECORD_FLAG_ENDS_STATEMENT;
    seal_newest_record(wal);
    wal->committed_length = wal->used_length;
}

minidb_status_t minidb_wal_commit(minidb_wal_t *wal) {
    if (wal->used_length == wal->committed_length) {
        return MINIDB_OK;  /* the statement logged nothing */
    }
    end_statement(wal);
    wal->stats.commits++;

    uint64_t now_ns = 0;
    if (wal->options.sync_mode == MINIDB_SYNC_GROUP) {
        now_ns = monotonic_now_ns();
    }
    if (wal->unsynced_commits++ == 0) {
        wal->oldest_unsynced_commit_ns = now_ns;
    }

    switch (wal->options.sync_mode) {
        case MINIDB_SYNC_FULL:
            return minidb_wal_sync(wal);

        case MINIDB_SYNC_GROUP: {
            uint64_t delay_ns = (uint64_t)wal->options.group_commit_delay_ms * 1000000ull;
            bool waited_long_enough = now_ns - wal->oldest_unsynced_commit_ns >= delay_ns;
            bool group_is_full = wal->committed_length >= wal->options.group_commit_bytes;
            if (waited_long_enough || group_is_full) {
                return minidb_wal_sync(wal);
            }
            return MINIDB_OK;
        }

        case MINIDB_SYNC_OFF:
            if (wal->committed_length >= wal->options.group_commit_bytes) {
                return write_committed(wal);
            }
            return MINIDB_OK;
    }
    return MINIDB_OK;
}

void minidb_wal_abort(minidb_wal_t *wal) {
    wal->used_length = wal->committed_length;
    wal->unsealed_record = NO_RECORD;
}

minidb_status_t minidb_wal_sync(minidb_wal_t *wal) {
    minidb_status_t status = write_committed(wal);
    if (status != MINIDB_OK) {
        return status;
    }
    return fsync_file(wal);
}

/* ---- Checkpoint records -------------------------------------------------- */

minidb_status_t minidb_wal_append_page_image(minidb_wal_t *wal, minidb_page_id_t page_id,
                                             const uint8_t *page_data) {
    if (wal->used_length != wal->committed_length) {
        return MINIDB_ERR_INVALID_ARGUMENT;  /* a statement is still open */
    }
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_PAGE_IMAGE,
        .object_id = page_id,
        .payload = page_data,
        .payload_length = MINIDB_PAGE_SIZE,
    };
    minidb_status_t status = minidb_wal_append(wal, &record);
    if (status != MINIDB_OK) {
        return status;
    }
    /* Each image stands alone, so the buffer can drain as it fills. */
    end_statement(wal);
    if (wal->committed_length >= wal->buffer_capacity / 2) {
        return write_committed(wal);
    }
    return MINIDB_OK;
}

minidb_status_t minidb_wal_finish_checkpoint(minidb_wal_t *wal, uint32_t page_count) {
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_CHECKPOINT_END,
        .key = page_count,
    };
    minidb_status_t status = minidb_wal_append(wal, &record);
    if (status != MINIDB_OK) {
        return status;
    }
    end_statement(wal);
    return minidb_wal_sync(wal);
}

minidb_status_t minidb_wal_truncate(minidb_wal_t *wal) {
    wal->used_length = 0;
    wal->committed_length = 0;
    wal->unsealed_record = NO_RECORD;
    wal->unsynced_commits = 0;
    wal->file_needs_fsync = false;

    if (ftruncate(wal->file_descriptor, WAL_HEADER_SIZE) != 0 ||
        fsync(wal->file_descriptor) != 0) {
        return MINIDB_ERR_IO;
    }
    wal->stats.fsyncs++;
    wal->file_size = WAL_HEADER_SIZE;
    return MINIDB_OK;
}

uint64_t minidb_wal_size(const minidb_wal_t *wal) {
    return wal->file_size - WAL_HEADER_SIZE + wal->used_length;
}

minidb_wal_stats_t minidb_wal_stats(const minidb_wal_t *wal) {
    return wal->stats;
}

/* ---- Reader -------------------------------------------------------------- */

minidb_status_t minidb_wal_reader_open(const char *path, minidb_wal_reader_t *reader) {
    memset(reader, 0, sizeof(*reader));

    int file_descriptor = open(path, O_RDONLY);
    if (file_descriptor < 0) {
        return MINIDB_OK;  /* no log yet: nothing to read */
    }

    minidb_status_t status = MINIDB_OK;
    struct stat file_info;
    if (fstat(file_descriptor, &file_info) != 0) {
        status = MINIDB_ERR_IO;
    } else if (file_info.st_size > 0) {
        reader->length = (size_t)file_info.st_size;
        reader->bytes = malloc(reader->length);
        if (reader->bytes == NULL) {
            status = MINIDB_ERR_NO_MEMORY;
        }
        size_t loaded = 0;
        while (status == MINIDB_OK && loaded < reader->length) {
            ssize_t result =
                pread(file_descriptor, reader->bytes + loaded, reader->length - loaded,
                      (off_t)loaded);
            if (result <= 0) {
                status = MINIDB_ERR_IO;
            } else {
                loaded += (size_t)result;
            }
        }
    }
    close(file_descriptor);

    if (status != MINIDB_OK) {
        minidb_wal_reader_close(reader);
        return status;
    }

    /* Without a valid header the file holds no records we can trust. */
    bool has_header = reader->length >= WAL_HEADER_SIZE &&
                      memcmp(reader->bytes, WAL_MAGIC, sizeof(WAL_MAGIC)) == 0 &&
                      read_u32(reader->bytes + sizeof(WAL_MAGIC)) == MINIDB_PAGE_SIZE;
    if (has_header) {
        reader->offset = WAL_HEADER_SIZE;
        reader->committed_length = WAL_HEADER_SIZE;
    } else {
        reader->length = 0;
    }
    return MINIDB_OK;
}

minidb_status_t minidb_wal_reader_next(minidb_wal_reader_t *reader,
                                       minidb_wal_record_t *out_record) {
    size_t remaining = reader->length - reader->offset;
    if (reader->length == 0 || remaining < RECORD_HEADER_SIZE) {
        return MINIDB_DONE;
    }
    const uint8_t *record = reader->bytes + reader->offset;

    uint32_t payload_length = read_u32(record + RECORD_PAYLOAD_LENGTH_OFFSET);
    uint8_t type = record[RECORD_TYPE_OFFSET];
    if (payload_length > RECORD_MAX_PAYLOAD || remaining - RECORD_HEADER_SIZE < payload_length ||
        type < MINIDB_WAL_INSERT || type > MINIDB_WAL_CHECKPOINT_END ||
        read_u32(record + RECORD_CHECKSUM_OFFSET) != record_checksum(record, payload_length)) {
        reader->offset = reader->length;  /* torn tail: stop here for good */
        return MINIDB_DONE;
    }

    out_record->type = (minidb_wal_record_type_t)type;
    out_record->ends_statement = (record[RECORD_FLAGS_OFFSET] & RECORD_FLAG_ENDS_STATEMENT) != 0;
    out_record->object_id = read_u32(record + RECORD_OBJECT_ID_OFFSET);
    memcpy(&out_record->key, record + RECORD_KEY_OFFSET, sizeof(out_record->key));
    out_record->payload = record + RECORD_HEADER_SIZE;
    out_record->payload_length = payload_length;

    reader->offset += RECORD_HEADER_SIZE + payload_length;
    if (out_record->ends_statement) {
        reader->committed_length = reader->offset;
    }
    return MINIDB_OK;
}

void minidb_wal_reader_close(minidb_wal_reader_t *reader) {
    free(reader->bytes);
    memset(reader, 0, sizeof(*reader));
}
