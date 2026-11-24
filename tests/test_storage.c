#include "test.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include "minidb/btree.h"
#include "minidb/failpoint.h"
#include "minidb/row.h"
#include "minidb/storage.h"

/*
 * Crash tests run the workload in a forked child that dies without closing
 * the database -- either by calling _exit() or by hitting an armed
 * failpoint -- and then reopen the files in the parent.
 */

#define ROW_TEXT_LENGTH 400

static minidb_options_t options_with(minidb_sync_mode_t sync_mode) {
    minidb_options_t options = minidb_options_default();
    options.sync_mode = sync_mode;
    options.group_commit_delay_ms = 60000;  /* GROUP: only flush when told to */
    options.group_commit_bytes = 64u << 20;
    return options;
}

static minidb_schema_t notes_schema(void) {
    minidb_schema_t schema;
    CHECK_OK(minidb_schema_init(&schema, "notes"));
    CHECK_OK(minidb_schema_add_column(&schema, "id", MINIDB_TYPE_INT, true));
    CHECK_OK(minidb_schema_add_column(&schema, "body", MINIDB_TYPE_TEXT, false));
    return schema;
}

/* The row for a key: (key, ROW_TEXT_LENGTH bytes derived from key and version). */
static uint32_t encode_note(const minidb_schema_t *schema, int64_t key, int version,
                            uint8_t *out_row) {
    char body[ROW_TEXT_LENGTH];
    for (int index = 0; index < ROW_TEXT_LENGTH; index++) {
        body[index] = (char)('a' + (key + version + index) % 26);
    }
    minidb_value_t values[2] = {minidb_value_int(key), minidb_value_text(body, ROW_TEXT_LENGTH)};
    minidb_row_encode(schema, values, out_row);
    return (uint32_t)minidb_row_encoded_size(schema, values);
}

static void insert_note(minidb_storage_t *storage, minidb_table_t *table, int64_t key) {
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t row_length = encode_note(&table->schema, key, 0, row);
    CHECK_OK(minidb_storage_insert(storage, table, key, row, row_length));
}

static minidb_table_t *create_notes(minidb_storage_t *storage) {
    minidb_schema_t schema = notes_schema();
    minidb_table_t *table;
    CHECK_OK(minidb_storage_create_table(storage, &schema, &table));
    CHECK_OK(minidb_storage_commit(storage));
    return table;
}

/* Checks the table holds exactly keys [0, expected_count), each at `version`. */
static void check_notes(minidb_storage_t *storage, int64_t expected_count, int version) {
    minidb_table_t *table = minidb_catalog_find(minidb_storage_catalog(storage), "notes");
    CHECK(table != NULL);

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, minidb_storage_pager(storage));
    CHECK_OK(minidb_cursor_first(&cursor, table->root_page));
    for (int64_t key = 0; key < expected_count; key++) {
        uint8_t expected_row[MINIDB_BTREE_MAX_PAYLOAD];
        uint32_t expected_length = encode_note(&table->schema, key, version, expected_row);
        uint32_t length;
        CHECK(minidb_cursor_is_valid(&cursor) && minidb_cursor_key(&cursor) == key);
        const uint8_t *row = minidb_cursor_payload(&cursor, &length);
        CHECK(length == expected_length && memcmp(row, expected_row, length) == 0);
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    CHECK(!minidb_cursor_is_valid(&cursor));
    minidb_cursor_close(&cursor);
}

static int64_t count_notes(minidb_storage_t *storage) {
    minidb_table_t *table = minidb_catalog_find(minidb_storage_catalog(storage), "notes");
    if (table == NULL) {
        return -1;
    }
    minidb_cursor_t cursor;
    int64_t count = 0;
    minidb_cursor_init(&cursor, minidb_storage_pager(storage));
    CHECK_OK(minidb_cursor_first(&cursor, table->root_page));
    while (minidb_cursor_is_valid(&cursor)) {
        count++;
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    minidb_cursor_close(&cursor);
    return count;
}

/* Runs `workload` in a child that never returns normally; yields its exit code. */
typedef void (*workload_t)(const char *path);
static int run_and_crash(workload_t workload, const char *path) {
    fflush(stdout);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        workload(path);
        _exit(0);  /* no close, no flush: the process just stops */
    }
    int wait_status;
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFEXITED(wait_status));
    return WEXITSTATUS(wait_status);
}

/* ---- Clean shutdown ------------------------------------------------------ */

static void test_clean_close_and_reopen(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_clean");
    minidb_options_t options = options_with(MINIDB_SYNC_GROUP);

    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    for (int64_t key = 0; key < 3000; key++) {
        insert_note(storage, table, key);
        CHECK_OK(minidb_storage_commit(storage));
    }
    CHECK_OK(minidb_storage_close(storage));

    /* close() checkpoints, so nothing is left to replay. */
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    CHECK(minidb_storage_stats(storage).recovered_statements == 0);
    check_notes(storage, 3000, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

/* ---- Crash with the log intact ------------------------------------------- */

static void workload_full_sync_inserts(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    for (int64_t key = 0; key < 500; key++) {
        insert_note(storage, table, key);
        CHECK_OK(minidb_storage_commit(storage));
    }
}

static void test_full_sync_loses_nothing(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_full");
    CHECK(run_and_crash(workload_full_sync_inserts, path) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    CHECK(minidb_storage_stats(storage).recovered_statements == 501);
    check_notes(storage, 500, 0);
    CHECK_OK(minidb_storage_close(storage));

    /* Recovery checkpointed: a second open has nothing to replay. */
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    CHECK(minidb_storage_stats(storage).recovered_statements == 0);
    check_notes(storage, 500, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void workload_group_commit_window(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_GROUP);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    for (int64_t key = 0; key < 1000; key++) {
        insert_note(storage, table, key);
        CHECK_OK(minidb_storage_commit(storage));
        if (key == 599) {
            CHECK_OK(minidb_storage_sync(storage));
        }
    }
}

static void test_group_commit_loses_only_the_unsynced_window(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_group");
    CHECK(run_and_crash(workload_group_commit_window, path) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_GROUP);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    /* Everything up to the sync survived; the 400 commits after it were
     * still in the buffer when the process died. */
    check_notes(storage, 600, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void workload_uncommitted_statement(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);

    /* One committed statement of 100 rows... */
    for (int64_t key = 0; key < 100; key++) {
        insert_note(storage, table, key);
    }
    CHECK_OK(minidb_storage_commit(storage));
    /* ...and one of 5000 rows (larger than the log buffer) that never commits. */
    for (int64_t key = 100; key < 5100; key++) {
        insert_note(storage, table, key);
    }
    CHECK_OK(minidb_storage_sync(storage));
}

static void test_statements_are_all_or_nothing(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_atomic");
    CHECK(run_and_crash(workload_uncommitted_statement, path) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    CHECK(minidb_storage_stats(storage).recovered_statements == 2);
    check_notes(storage, 100, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void workload_updates_deletes_and_drop(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];

    for (int64_t key = 0; key < 400; key++) {
        insert_note(storage, table, key);
    }
    CHECK_OK(minidb_storage_commit(storage));
    for (int64_t key = 0; key < 400; key++) {
        uint32_t row_length = encode_note(&table->schema, key, 1, row);
        CHECK_OK(minidb_storage_update(storage, table, key, row, row_length));
    }
    CHECK_OK(minidb_storage_commit(storage));
    for (int64_t key = 300; key < 400; key++) {
        CHECK_OK(minidb_storage_delete(storage, table, key));
    }
    CHECK_OK(minidb_storage_commit(storage));

    /* A second table that is created, filled and dropped again. */
    minidb_schema_t scratch_schema = notes_schema();
    memcpy(scratch_schema.table_name, "scratch", sizeof("scratch"));
    minidb_table_t *scratch;
    CHECK_OK(minidb_storage_create_table(storage, &scratch_schema, &scratch));
    CHECK_OK(minidb_storage_commit(storage));
    insert_note(storage, scratch, 1);
    CHECK_OK(minidb_storage_commit(storage));
    CHECK_OK(minidb_storage_drop_table(storage, scratch));
    CHECK_OK(minidb_storage_commit(storage));
}

static void test_replays_updates_deletes_and_drops(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_mixed");
    CHECK(run_and_crash(workload_updates_deletes_and_drop, path) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    check_notes(storage, 300, 1);
    CHECK(minidb_catalog_find(minidb_storage_catalog(storage), "scratch") == NULL);
    CHECK(minidb_catalog_table_count(minidb_storage_catalog(storage)) == 1);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void test_torn_log_tail_drops_only_the_last_statement(void) {
    char path[256];
    char wal_path[300];
    test_scratch_path(path, sizeof(path), "storage_torn");
    snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    CHECK(run_and_crash(workload_full_sync_inserts, path) == 0);

    struct stat info;
    CHECK(stat(wal_path, &info) == 0);
    CHECK(truncate(wal_path, info.st_size - 5) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    check_notes(storage, 499, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

/* ---- Crash in the middle of a checkpoint --------------------------------- */

static minidb_failpoint_t checkpoint_failpoint;

static void workload_crash_during_checkpoint(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_GROUP);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];

    /* First generation, checkpointed cleanly: this is what the file holds. */
    for (int64_t key = 0; key < 3000; key++) {
        insert_note(storage, table, key);
        CHECK_OK(minidb_storage_commit(storage));
    }
    CHECK_OK(minidb_storage_checkpoint(storage));

    /* Second generation rewrites every page, then dies while checkpointing. */
    for (int64_t key = 0; key < 3000; key++) {
        uint32_t row_length = encode_note(&table->schema, key, 1, row);
        CHECK_OK(minidb_storage_update(storage, table, key, row, row_length));
        CHECK_OK(minidb_storage_commit(storage));
    }
    for (int64_t key = 3000; key < 6000; key++) {
        uint32_t row_length = encode_note(&table->schema, key, 1, row);
        CHECK_OK(minidb_storage_insert(storage, table, key, row, row_length));
        CHECK_OK(minidb_storage_commit(storage));
    }
    CHECK_OK(minidb_storage_sync(storage));

    minidb_failpoint_arm(checkpoint_failpoint);
    CHECK_OK(minidb_storage_checkpoint(storage));
    _exit(1);  /* the failpoint should have fired */
}

static void check_checkpoint_crash(minidb_failpoint_t failpoint, const char *name) {
    char path[256];
    test_scratch_path(path, sizeof(path), name);
    checkpoint_failpoint = failpoint;
    CHECK(run_and_crash(workload_crash_during_checkpoint, path) == MINIDB_FAILPOINT_EXIT_CODE);

    minidb_options_t options = options_with(MINIDB_SYNC_GROUP);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    check_notes(storage, 6000, 1);

    /* The recovered database is fully usable. */
    minidb_table_t *table = minidb_catalog_find(minidb_storage_catalog(storage), "notes");
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t row_length = encode_note(&table->schema, 6000, 1, row);
    CHECK_OK(minidb_storage_insert(storage, table, 6000, row, row_length));
    CHECK_OK(minidb_storage_commit(storage));
    CHECK_OK(minidb_storage_close(storage));

    CHECK_OK(minidb_storage_open(path, &options, &storage));
    check_notes(storage, 6001, 1);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void test_crash_while_logging_page_images(void) {
    check_checkpoint_crash(MINIDB_FAILPOINT_CHECKPOINT_HALF_LOGGED, "storage_ckpt_half_logged");
}

static void test_crash_after_logging_page_images(void) {
    check_checkpoint_crash(MINIDB_FAILPOINT_CHECKPOINT_LOGGED, "storage_ckpt_logged");
}

static void test_crash_while_overwriting_the_database_file(void) {
    check_checkpoint_crash(MINIDB_FAILPOINT_CHECKPOINT_HALF_WRITTEN, "storage_ckpt_half_written");
}

static void test_crash_before_emptying_the_log(void) {
    check_checkpoint_crash(MINIDB_FAILPOINT_CHECKPOINT_WRITTEN, "storage_ckpt_written");
}

/* ---- Automatic checkpoints ----------------------------------------------- */

static void workload_small_log_limit(const char *path) {
    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    options.checkpoint_wal_bytes = 64 * 1024;
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    for (int64_t key = 0; key < 2000; key++) {
        insert_note(storage, table, key);
        CHECK_OK(minidb_storage_commit(storage));
    }
    /* ~430 bytes per record against a 64 KB limit: many checkpoints ran. */
    CHECK(minidb_storage_stats(storage).checkpoints >= 10);
}

static void test_log_is_checkpointed_when_it_grows(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_auto_ckpt");
    CHECK(run_and_crash(workload_small_log_limit, path) == 0);

    minidb_options_t options = options_with(MINIDB_SYNC_FULL);
    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    /* Only the statements after the last checkpoint needed replaying. */
    CHECK(minidb_storage_stats(storage).recovered_statements < 200);
    check_notes(storage, 2000, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

static void test_abort_leaves_no_trace_in_the_log(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "storage_abort");
    minidb_options_t options = options_with(MINIDB_SYNC_FULL);

    minidb_storage_t *storage;
    CHECK_OK(minidb_storage_open(path, &options, &storage));
    minidb_table_t *table = create_notes(storage);
    insert_note(storage, table, 0);
    CHECK_OK(minidb_storage_commit(storage));

    /* A statement that fails half-way: undo its row, then abort its records. */
    insert_note(storage, table, 1);
    uint8_t row[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t row_length = encode_note(&table->schema, 0, 0, row);
    CHECK_STATUS(MINIDB_ERR_DUPLICATE_KEY,
                 minidb_storage_insert(storage, table, 0, row, row_length));
    CHECK_OK(minidb_storage_delete(storage, table, 1));
    minidb_storage_abort(storage);
    CHECK(count_notes(storage) == 1);
    CHECK_OK(minidb_storage_close(storage));

    CHECK_OK(minidb_storage_open(path, &options, &storage));
    check_notes(storage, 1, 0);
    CHECK_OK(minidb_storage_close(storage));
    test_remove_database(path);
}

int main(void) {
    RUN(test_clean_close_and_reopen);
    RUN(test_full_sync_loses_nothing);
    RUN(test_group_commit_loses_only_the_unsynced_window);
    RUN(test_statements_are_all_or_nothing);
    RUN(test_replays_updates_deletes_and_drops);
    RUN(test_torn_log_tail_drops_only_the_last_statement);
    RUN(test_crash_while_logging_page_images);
    RUN(test_crash_after_logging_page_images);
    RUN(test_crash_while_overwriting_the_database_file);
    RUN(test_crash_before_emptying_the_log);
    RUN(test_log_is_checkpointed_when_it_grows);
    RUN(test_abort_leaves_no_trace_in_the_log);
    return EXIT_SUCCESS;
}
