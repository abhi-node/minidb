/*
 * minidb benchmarks. Everything goes through the public SQL API, so the
 * numbers include tokenizing, parsing, planning and execution.
 *
 *   minidb_bench insert [--rows N] [--sync full|group|off] [--delay-ms D]
 *                       [--random] [--timeline FILE]
 *   minidb_bench lookup [--rows N] [--lookups M]
 *   minidb_bench join   [--rows N] [--skip-nested-loop]
 *
 * Common options: --db PATH, --cache-pages N, --profile FILE
 *
 * Each run prints one line of key=value pairs, which scripts/ parse.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "minidb/minidb.h"
#include "profiler.h"

typedef struct {
    const char *workload;
    const char *database_path;
    const char *profile_path;   /* NULL: do not profile */
    const char *timeline_path;  /* NULL: do not record a timeline */
    int64_t row_count;
    int64_t lookup_count;
    bool use_random_keys;
    bool skip_nested_loop;
    minidb_options_t database;
} bench_options_t;

/* ---- Helpers ------------------------------------------------------------- */

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void die(const char *message, const char *detail) {
    fprintf(stderr, "minidb_bench: %s%s%s\n", message, detail != NULL ? ": " : "",
            detail != NULL ? detail : "");
    exit(EXIT_FAILURE);
}

static void run(minidb_t *db, const char *sql, minidb_row_callback_t on_row, void *context) {
    if (minidb_exec(db, sql, on_row, context) != MINIDB_OK) {
        die(minidb_error_message(db), sql);
    }
}

static void remove_database(const char *path) {
    char wal_path[1024];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    unlink(path);
    unlink(wal_path);
}

static minidb_t *open_fresh_database(const bench_options_t *options) {
    minidb_t *db;
    remove_database(options->database_path);
    if (minidb_open(options->database_path, &options->database, &db) != MINIDB_OK) {
        die("cannot open database", options->database_path);
    }
    return db;
}

/* A fixed bijection on 64-bit integers: scatters 0, 1, 2, ... across the key space. */
static int64_t scramble(int64_t sequence) {
    return (int64_t)(((uint64_t)sequence * 0x9E3779B97F4A7C15ull) >> 1);
}

static const char *sync_mode_name(minidb_sync_mode_t mode) {
    switch (mode) {
        case MINIDB_SYNC_FULL:  return "full";
        case MINIDB_SYNC_GROUP: return "group";
        case MINIDB_SYNC_OFF:   return "off";
    }
    return "?";
}

static void start_profiler_if_requested(const bench_options_t *options) {
    if (options->profile_path != NULL && !profiler_start()) {
        die("cannot start the profiler", NULL);
    }
}

static void stop_profiler_if_requested(const bench_options_t *options) {
    if (options->profile_path != NULL && !profiler_stop(options->profile_path)) {
        die("cannot write profile", options->profile_path);
    }
}

/* ---- insert: one row per statement, one statement per commit ------------- */

#define TIMELINE_STEP_ROWS 50000

static void insert_account(minidb_t *db, int64_t id) {
    char sql[160];
    snprintf(sql, sizeof(sql),
             "INSERT INTO accounts VALUES (%" PRId64 ", 'user%" PRId64 "', %" PRId64 ".25, TRUE)",
             id, id, id % 1000);
    run(db, sql, NULL, NULL);
}

static void bench_insert(const bench_options_t *options) {
    minidb_t *db = open_fresh_database(options);
    run(db, "CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance DECIMAL, active BOOL)",
        NULL, NULL);

    FILE *timeline = NULL;
    if (options->timeline_path != NULL) {
        timeline = fopen(options->timeline_path, "w");
        if (timeline == NULL) {
            die("cannot write timeline", options->timeline_path);
        }
        fprintf(timeline, "rows,seconds\n");
    }

    start_profiler_if_requested(options);
    double start = now_seconds();
    for (int64_t sequence = 0; sequence < options->row_count; sequence++) {
        insert_account(db, options->use_random_keys ? scramble(sequence) : sequence);
        if (timeline != NULL && (sequence + 1) % TIMELINE_STEP_ROWS == 0) {
            fprintf(timeline, "%" PRId64 ",%.6f\n", sequence + 1, now_seconds() - start);
        }
    }
    /* The clock stops only when every row is on disk. */
    if (minidb_sync(db) != MINIDB_OK) {
        die("sync failed", NULL);
    }
    double seconds = now_seconds() - start;
    stop_profiler_if_requested(options);

    if (timeline != NULL) {
        fclose(timeline);
    }

    minidb_storage_stats_t stats = minidb_stats(db);
    printf("workload=insert sync=%s delay_ms=%u keys=%s rows=%" PRId64
           " seconds=%.4f rows_per_sec=%.0f fsyncs=%" PRIu64 " wal_mb=%.1f checkpoints=%" PRIu64
           "\n",
           sync_mode_name(options->database.sync_mode), options->database.group_commit_delay_ms,
           options->use_random_keys ? "random" : "ascending", options->row_count, seconds,
           (double)options->row_count / seconds, stats.wal.fsyncs,
           (double)stats.wal.bytes_written / (1024.0 * 1024.0), stats.checkpoints);

    minidb_close(db);
    remove_database(options->database_path);
}

/* ---- lookup: SELECT ... WHERE id = ? on a table of N rows ----------------- */

/* Loads `row_count` accounts as fast as possible; durability is irrelevant here. */
static void load_accounts(minidb_t *db, int64_t row_count) {
    run(db, "CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance DECIMAL, active BOOL)",
        NULL, NULL);
    for (int64_t id = 0; id < row_count; id++) {
        insert_account(db, id);
    }
    if (minidb_checkpoint(db) != MINIDB_OK) {
        die("checkpoint failed", NULL);
    }
}

static bool count_row(void *context, const minidb_row_t *row) {
    (void)row;
    (*(int64_t *)context)++;
    return true;
}

static uint64_t page_fetches(const minidb_t *db) {
    minidb_storage_stats_t stats = minidb_stats(db);
    return stats.pager.cache_hits + stats.pager.cache_misses;
}

static void bench_lookup(const bench_options_t *options) {
    bench_options_t load_options = *options;
    load_options.database.sync_mode = MINIDB_SYNC_OFF;
    minidb_t *db = open_fresh_database(&load_options);
    load_accounts(db, options->row_count);

    int64_t found_count = 0;
    uint64_t fetches_before = page_fetches(db);

    start_profiler_if_requested(options);
    double start = now_seconds();
    for (int64_t lookup = 0; lookup < options->lookup_count; lookup++) {
        char sql[96];
        int64_t id = (int64_t)((uint64_t)scramble(lookup) % (uint64_t)options->row_count);
        snprintf(sql, sizeof(sql), "SELECT balance FROM accounts WHERE id = %" PRId64, id);
        run(db, sql, count_row, &found_count);
    }
    double seconds = now_seconds() - start;
    stop_profiler_if_requested(options);

    if (found_count != options->lookup_count) {
        die("a lookup did not return exactly one row", NULL);
    }
    double fetches_per_lookup =
        (double)(page_fetches(db) - fetches_before) / (double)options->lookup_count;

    printf("workload=lookup rows=%" PRId64 " lookups=%" PRId64
           " seconds=%.4f ns_per_lookup=%.0f lookups_per_sec=%.0f pages_per_lookup=%.2f\n",
           options->row_count, options->lookup_count, seconds,
           seconds * 1e9 / (double)options->lookup_count,
           (double)options->lookup_count / seconds, fetches_per_lookup);

    minidb_close(db);
    remove_database(options->database_path);
}

/* ---- join: every order with its user ------------------------------------- */

static double time_query(minidb_t *db, const char *sql, int64_t expected_rows) {
    int64_t row_count = 0;
    double start = now_seconds();
    run(db, sql, count_row, &row_count);
    double seconds = now_seconds() - start;
    if (row_count != expected_rows) {
        die("join returned the wrong number of rows", sql);
    }
    return seconds;
}

static void bench_join(const bench_options_t *options) {
    bench_options_t load_options = *options;
    load_options.database.sync_mode = MINIDB_SYNC_OFF;
    minidb_t *db = open_fresh_database(&load_options);
    int64_t row_count = options->row_count;

    run(db, "CREATE TABLE users (id INT PRIMARY KEY, name TEXT);"
            "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, total DECIMAL);",
        NULL, NULL);
    for (int64_t id = 0; id < row_count; id++) {
        char sql[192];
        int64_t user_id = (int64_t)((uint64_t)scramble(id) % (uint64_t)row_count);
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES (%" PRId64 ", 'user%" PRId64 "');"
                 "INSERT INTO orders VALUES (%" PRId64 ", %" PRId64 ", %" PRId64 ".5);",
                 id, id, id, user_id, id % 500);
        run(db, sql, NULL, NULL);
    }

    start_profiler_if_requested(options);
    /* users.id is a primary key, so this probes the users B+-tree per order. */
    double index_seconds = time_query(
        db, "SELECT o.id, u.name FROM orders o JOIN users u ON u.id = o.user_id", row_count);
    /* orders.user_id is not a key, so this rescans orders for every user. */
    double nested_seconds = -1.0;
    if (!options->skip_nested_loop) {
        nested_seconds = time_query(
            db, "SELECT o.id, u.name FROM users u JOIN orders o ON o.user_id = u.id", row_count);
    }
    stop_profiler_if_requested(options);

    printf("workload=join rows=%" PRId64 " index_join_seconds=%.6f nested_loop_seconds=%.6f\n",
           row_count, index_seconds, nested_seconds);

    minidb_close(db);
    remove_database(options->database_path);
}

/* ---- Command line -------------------------------------------------------- */

static void usage(void) {
    fputs("usage: minidb_bench <insert|lookup|join> [options]\n"
          "  --rows N            rows to insert or load (default 1000000)\n"
          "  --lookups M         point lookups to run (default 200000)\n"
          "  --sync MODE         full | group | off (default group)\n"
          "  --delay-ms D        group-commit fsync delay (default 5)\n"
          "  --random            insert keys in scattered order instead of ascending\n"
          "  --skip-nested-loop  join: only time the index join\n"
          "  --cache-pages N     page cache size in 4 KB pages\n"
          "  --db PATH           database file (default /tmp/minidb_bench.db)\n"
          "  --timeline FILE     insert: write rows,seconds every 50000 rows\n"
          "  --profile FILE      write stack samples for scripts/flamegraph.py\n",
          stderr);
    exit(EXIT_FAILURE);
}

static minidb_sync_mode_t parse_sync_mode(const char *text) {
    if (strcmp(text, "full") == 0) {
        return MINIDB_SYNC_FULL;
    }
    if (strcmp(text, "group") == 0) {
        return MINIDB_SYNC_GROUP;
    }
    if (strcmp(text, "off") == 0) {
        return MINIDB_SYNC_OFF;
    }
    usage();
    return MINIDB_SYNC_GROUP;
}

static bench_options_t parse_options(int argc, char **argv) {
    if (argc < 2) {
        usage();
    }
    bench_options_t options = {
        .workload = argv[1],
        .database_path = "/tmp/minidb_bench.db",
        .row_count = 1000000,
        .lookup_count = 200000,
        .database = minidb_options_default(),
    };

    for (int index = 2; index < argc; index++) {
        const char *flag = argv[index];
        bool has_value = index + 1 < argc;

        if (strcmp(flag, "--random") == 0) {
            options.use_random_keys = true;
        } else if (strcmp(flag, "--skip-nested-loop") == 0) {
            options.skip_nested_loop = true;
        } else if (!has_value) {
            usage();
        } else if (strcmp(flag, "--rows") == 0) {
            options.row_count = atoll(argv[++index]);
        } else if (strcmp(flag, "--lookups") == 0) {
            options.lookup_count = atoll(argv[++index]);
        } else if (strcmp(flag, "--sync") == 0) {
            options.database.sync_mode = parse_sync_mode(argv[++index]);
        } else if (strcmp(flag, "--delay-ms") == 0) {
            options.database.group_commit_delay_ms = (uint32_t)atoi(argv[++index]);
        } else if (strcmp(flag, "--cache-pages") == 0) {
            options.database.cache_pages = (uint32_t)atoi(argv[++index]);
        } else if (strcmp(flag, "--db") == 0) {
            options.database_path = argv[++index];
        } else if (strcmp(flag, "--timeline") == 0) {
            options.timeline_path = argv[++index];
        } else if (strcmp(flag, "--profile") == 0) {
            options.profile_path = argv[++index];
        } else {
            usage();
        }
    }
    if (options.row_count <= 0 || options.lookup_count <= 0) {
        usage();
    }
    return options;
}

int main(int argc, char **argv) {
    bench_options_t options = parse_options(argc, argv);

    if (strcmp(options.workload, "insert") == 0) {
        bench_insert(&options);
    } else if (strcmp(options.workload, "lookup") == 0) {
        bench_lookup(&options);
    } else if (strcmp(options.workload, "join") == 0) {
        bench_join(&options);
    } else {
        usage();
    }
    return EXIT_SUCCESS;
}
