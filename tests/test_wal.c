#include "test.h"

#include <sys/stat.h>

#include "minidb/wal.h"

static const minidb_wal_options_t FULL_SYNC = {MINIDB_SYNC_FULL, 0, 1 << 20};

static void wal_path_for(char *out_path, size_t capacity, const char *name) {
    char database_path[256];
    test_scratch_path(database_path, sizeof(database_path), name);
    snprintf(out_path, capacity, "%s.wal", database_path);
}

static minidb_wal_record_t insert_record(int64_t key, const char *text) {
    minidb_wal_record_t record = {
        .type = MINIDB_WAL_INSERT,
        .object_id = 7,
        .key = key,
        .payload = (const uint8_t *)text,
        .payload_length = (uint32_t)strlen(text),
    };
    return record;
}

static off_t file_size(const char *path) {
    struct stat info;
    CHECK(stat(path, &info) == 0);
    return info.st_size;
}

/* Reads the whole log and returns how many records it holds. */
static int count_records(const char *path, int *out_statement_count) {
    minidb_wal_reader_t reader;
    minidb_wal_record_t record;
    int record_count = 0;
    int statement_count = 0;
    CHECK_OK(minidb_wal_reader_open(path, &reader));
    while (minidb_wal_reader_next(&reader, &record) == MINIDB_OK) {
        record_count++;
        statement_count += record.ends_statement ? 1 : 0;
    }
    minidb_wal_reader_close(&reader);
    if (out_statement_count != NULL) {
        *out_statement_count = statement_count;
    }
    return record_count;
}

static void test_records_round_trip(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_round_trip");

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    minidb_wal_record_t first = insert_record(1, "alpha");
    minidb_wal_record_t second = insert_record(2, "beta");
    minidb_wal_record_t removal = {.type = MINIDB_WAL_DELETE, .object_id = 7, .key = 1};
    CHECK_OK(minidb_wal_append(wal, &first));
    CHECK_OK(minidb_wal_append(wal, &second));
    CHECK_OK(minidb_wal_commit(wal));
    CHECK_OK(minidb_wal_append(wal, &removal));
    CHECK_OK(minidb_wal_commit(wal));
    minidb_wal_close(wal);

    minidb_wal_reader_t reader;
    minidb_wal_record_t record;
    CHECK_OK(minidb_wal_reader_open(path, &reader));

    CHECK_OK(minidb_wal_reader_next(&reader, &record));
    CHECK(record.type == MINIDB_WAL_INSERT && record.key == 1 && record.object_id == 7);
    CHECK(record.payload_length == 5 && memcmp(record.payload, "alpha", 5) == 0);
    CHECK(!record.ends_statement);

    CHECK_OK(minidb_wal_reader_next(&reader, &record));
    CHECK(record.key == 2 && record.ends_statement);

    CHECK_OK(minidb_wal_reader_next(&reader, &record));
    CHECK(record.type == MINIDB_WAL_DELETE && record.payload_length == 0);
    CHECK(record.ends_statement);

    CHECK_STATUS(MINIDB_DONE, minidb_wal_reader_next(&reader, &record));
    minidb_wal_reader_close(&reader);
    unlink(path);
}

static void test_full_sync_fsyncs_every_commit(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_full");

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    uint64_t fsyncs_at_start = minidb_wal_stats(wal).fsyncs;
    for (int64_t key = 0; key < 10; key++) {
        minidb_wal_record_t record = insert_record(key, "row");
        CHECK_OK(minidb_wal_append(wal, &record));
        CHECK_OK(minidb_wal_commit(wal));
    }
    CHECK(minidb_wal_stats(wal).fsyncs - fsyncs_at_start == 10);
    minidb_wal_close(wal);
    unlink(path);
}

static void test_group_commit_shares_one_fsync(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_group");
    minidb_wal_options_t options = {MINIDB_SYNC_GROUP, 60000, 1 << 20};

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &options, &wal));
    uint64_t fsyncs_at_start = minidb_wal_stats(wal).fsyncs;
    for (int64_t key = 0; key < 1000; key++) {
        minidb_wal_record_t record = insert_record(key, "row");
        CHECK_OK(minidb_wal_append(wal, &record));
        CHECK_OK(minidb_wal_commit(wal));
    }
    /* The delay has not elapsed and the group is not full: nothing on disk. */
    CHECK(minidb_wal_stats(wal).fsyncs == fsyncs_at_start);
    CHECK(count_records(path, NULL) == 0);

    CHECK_OK(minidb_wal_sync(wal));
    CHECK(minidb_wal_stats(wal).fsyncs == fsyncs_at_start + 1);
    CHECK(count_records(path, NULL) == 1000);
    minidb_wal_close(wal);
    unlink(path);
}

static void test_group_commit_flushes_when_delay_elapses_or_group_fills(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_group_triggers");
    minidb_wal_record_t record = insert_record(1, "row");
    minidb_wal_t *wal;

    /* A zero delay behaves like FULL. */
    minidb_wal_options_t no_delay = {MINIDB_SYNC_GROUP, 0, 1 << 20};
    CHECK_OK(minidb_wal_open(path, &no_delay, &wal));
    CHECK_OK(minidb_wal_append(wal, &record));
    CHECK_OK(minidb_wal_commit(wal));
    CHECK(count_records(path, NULL) == 1);
    minidb_wal_close(wal);
    unlink(path);

    /* A long delay still flushes once the group reaches its byte limit. */
    minidb_wal_options_t small_group = {MINIDB_SYNC_GROUP, 60000, 256};
    CHECK_OK(minidb_wal_open(path, &small_group, &wal));
    for (int count = 0; count < 20; count++) {
        CHECK_OK(minidb_wal_append(wal, &record));
        CHECK_OK(minidb_wal_commit(wal));
    }
    CHECK(count_records(path, NULL) >= 10);
    minidb_wal_close(wal);
    unlink(path);
}

static void test_abort_discards_the_open_statement(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_abort");

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    minidb_wal_record_t kept = insert_record(1, "kept");
    minidb_wal_record_t dropped = insert_record(2, "dropped");
    CHECK_OK(minidb_wal_append(wal, &kept));
    CHECK_OK(minidb_wal_commit(wal));
    CHECK_OK(minidb_wal_append(wal, &dropped));
    CHECK_OK(minidb_wal_append(wal, &dropped));
    minidb_wal_abort(wal);
    CHECK_OK(minidb_wal_append(wal, &kept));
    CHECK_OK(minidb_wal_commit(wal));
    minidb_wal_close(wal);

    int statement_count;
    CHECK(count_records(path, &statement_count) == 2 && statement_count == 2);
    unlink(path);
}

static void test_statement_larger_than_the_buffer(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_large");
    enum { RECORD_COUNT = 5000 };
    char text[1001];
    memset(text, 'x', 1000);
    text[1000] = '\0';

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    for (int64_t key = 0; key < RECORD_COUNT; key++) {
        minidb_wal_record_t record = insert_record(key, text);
        CHECK_OK(minidb_wal_append(wal, &record));
    }
    /* 5 MB of one uncommitted statement: none of it may be on disk yet. */
    CHECK(count_records(path, NULL) == 0);
    CHECK_OK(minidb_wal_commit(wal));
    minidb_wal_close(wal);

    int statement_count;
    CHECK(count_records(path, &statement_count) == RECORD_COUNT && statement_count == 1);
    unlink(path);
}

static void test_torn_tail_is_ignored_and_cut_off(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_torn");

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    for (int64_t key = 0; key < 3; key++) {
        minidb_wal_record_t record = insert_record(key, "payload");
        CHECK_OK(minidb_wal_append(wal, &record));
        CHECK_OK(minidb_wal_commit(wal));
    }
    minidb_wal_close(wal);

    /* Tear the last record: a crash in the middle of a write. */
    off_t intact_size = file_size(path);
    CHECK(truncate(path, intact_size - 3) == 0);
    CHECK(count_records(path, NULL) == 2);

    /* Reopening removes the debris, so new records extend a clean prefix. */
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    minidb_wal_record_t record = insert_record(99, "after");
    CHECK_OK(minidb_wal_append(wal, &record));
    CHECK_OK(minidb_wal_commit(wal));
    minidb_wal_close(wal);
    CHECK(count_records(path, NULL) == 3);
    unlink(path);
}

static void test_corrupted_byte_stops_the_reader(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_corrupt");

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    for (int64_t key = 0; key < 4; key++) {
        minidb_wal_record_t record = insert_record(key, "payload");
        CHECK_OK(minidb_wal_append(wal, &record));
        CHECK_OK(minidb_wal_commit(wal));
    }
    minidb_wal_close(wal);

    /* Flip one payload byte in the third record (16-byte file header, then
     * 24-byte record header + 7-byte payload per record). */
    FILE *file = fopen(path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, 16 + 2 * 31 + 24 + 3, SEEK_SET) == 0);
    CHECK(fputc('X', file) != EOF);
    fclose(file);

    CHECK(count_records(path, NULL) == 2);
    unlink(path);
}

static void test_checkpoint_records_and_truncate(void) {
    char path[300];
    wal_path_for(path, sizeof(path), "wal_checkpoint");
    uint8_t page[MINIDB_PAGE_SIZE];
    memset(page, 0xAB, sizeof(page));

    minidb_wal_t *wal;
    CHECK_OK(minidb_wal_open(path, &FULL_SYNC, &wal));
    for (minidb_page_id_t page_id = 0; page_id < 600; page_id++) {
        CHECK_OK(minidb_wal_append_page_image(wal, page_id, page));
    }
    CHECK_OK(minidb_wal_finish_checkpoint(wal, 600));
    CHECK(minidb_wal_size(wal) > 600 * MINIDB_PAGE_SIZE);

    minidb_wal_reader_t reader;
    minidb_wal_record_t record;
    CHECK_OK(minidb_wal_reader_open(path, &reader));
    for (minidb_page_id_t page_id = 0; page_id < 600; page_id++) {
        CHECK_OK(minidb_wal_reader_next(&reader, &record));
        CHECK(record.type == MINIDB_WAL_PAGE_IMAGE && record.object_id == page_id);
        CHECK(record.payload_length == MINIDB_PAGE_SIZE && record.payload[100] == 0xAB);
    }
    CHECK_OK(minidb_wal_reader_next(&reader, &record));
    CHECK(record.type == MINIDB_WAL_CHECKPOINT_END && record.key == 600);
    minidb_wal_reader_close(&reader);

    CHECK_OK(minidb_wal_truncate(wal));
    CHECK(minidb_wal_size(wal) == 0);
    CHECK(count_records(path, NULL) == 0);
    minidb_wal_close(wal);
    unlink(path);
}

int main(void) {
    RUN(test_records_round_trip);
    RUN(test_full_sync_fsyncs_every_commit);
    RUN(test_group_commit_shares_one_fsync);
    RUN(test_group_commit_flushes_when_delay_elapses_or_group_fills);
    RUN(test_abort_discards_the_open_statement);
    RUN(test_statement_larger_than_the_buffer);
    RUN(test_torn_tail_is_ignored_and_cut_off);
    RUN(test_corrupted_byte_stops_the_reader);
    RUN(test_checkpoint_records_and_truncate);
    return EXIT_SUCCESS;
}
