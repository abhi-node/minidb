#include "test.h"

#include <signal.h>
#include <sys/wait.h>

#include "minidb/minidb.h"

/* End-to-end tests: SQL text in, result rows out, through the public API. */

static char result_text[1 << 16];

static bool append_row(void *context, const minidb_row_t *row) {
    (void)context;
    FILE *stream = fmemopen(result_text + strlen(result_text),
                            sizeof(result_text) - strlen(result_text), "w");
    CHECK(stream != NULL);
    for (uint32_t column = 0; column < row->column_count; column++) {
        if (column > 0) {
            fputc('|', stream);
        }
        minidb_value_print(&row->values[column], stream);
    }
    fputc('\n', stream);
    fclose(stream);
    return true;
}

/* Runs `sql` and returns its rows as "a|b\nc|d\n". */
static const char *query(minidb_t *db, const char *sql) {
    result_text[0] = '\0';
    minidb_status_t status = minidb_exec(db, sql, append_row, NULL);
    if (status != MINIDB_OK) {
        fprintf(stderr, "\"%s\" failed: %s\n", sql, minidb_error_message(db));
        exit(EXIT_FAILURE);
    }
    return result_text;
}

static void check_query(minidb_t *db, const char *sql, const char *expected) {
    const char *actual = query(db, sql);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "\"%s\"\n--- got:\n%s--- expected:\n%s", sql, actual, expected);
        exit(EXIT_FAILURE);
    }
}

static void check_error(minidb_t *db, const char *sql, minidb_status_t expected_status,
                        const char *expected_fragment) {
    minidb_status_t status = minidb_exec(db, sql, NULL, NULL);
    if (status != expected_status ||
        strstr(minidb_error_message(db), expected_fragment) == NULL) {
        fprintf(stderr, "\"%s\": expected %s \"%s\", got %s \"%s\"\n", sql,
                minidb_status_name(expected_status), expected_fragment,
                minidb_status_name(status), minidb_error_message(db));
        exit(EXIT_FAILURE);
    }
}

static minidb_t *open_database(const char *path) {
    minidb_t *db;
    CHECK_OK(minidb_open(path, NULL, &db));
    return db;
}

static minidb_t *open_shop(char *path, size_t capacity, const char *name) {
    test_scratch_path(path, capacity, name);
    minidb_t *db = open_database(path);
    query(db, "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT, active BOOL);"
              "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, total DECIMAL);"
              "INSERT INTO users VALUES (1, 'Ada', 36, TRUE), (2, 'Grace', 45, TRUE),"
              "                         (3, 'Linus', 28, FALSE), (4, 'Margaret', 52, TRUE);"
              "INSERT INTO orders VALUES (10, 1, 99.5), (11, 1, 20), (12, 3, 7.25),"
              "                          (13, 9, 1), (14, 4, 300);");
    return db;
}

static void close_shop(minidb_t *db, const char *path) {
    CHECK_OK(minidb_close(db));
    test_remove_database(path);
}

/* ---- SELECT -------------------------------------------------------------- */

static void test_select_filter_and_projection(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_select");

    check_query(db, "SELECT * FROM users WHERE id = 2", "2|Grace|45|true\n");
    check_query(db, "SELECT name FROM users WHERE age > 30 AND active = TRUE",
                "Ada\nGrace\nMargaret\n");
    check_query(db, "SELECT name, age + 1, age * 1.5 FROM users WHERE name LIKE '%a%' AND id < 3",
                "Ada|37|54\nGrace|46|67.5\n");
    check_query(db, "SELECT id FROM users WHERE NOT active OR age >= 50", "3\n4\n");
    check_query(db, "SELECT id FROM users WHERE id > 1 AND id <= 3", "2\n3\n");
    check_query(db, "SELECT id FROM users WHERE 3 > id AND id != 1", "2\n");
    check_query(db, "SELECT id FROM users WHERE id > 100", "");
    check_query(db, "SELECT id FROM users WHERE id = 2 AND id = 3", "");
    close_shop(db, path);
}

static void test_order_by_and_limit(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_order");

    check_query(db, "SELECT name FROM users ORDER BY age DESC", "Margaret\nGrace\nAda\nLinus\n");
    check_query(db, "SELECT name FROM users ORDER BY name LIMIT 2", "Ada\nGrace\n");
    check_query(db, "SELECT id FROM users LIMIT 0", "");
    check_query(db, "SELECT user_id, total FROM orders ORDER BY user_id, total DESC",
                "1|99.5\n1|20\n3|7.25\n4|300\n9|1\n");
    check_query(db, "SELECT id FROM orders ORDER BY total * -1 LIMIT 1", "14\n");
    close_shop(db, path);
}

static void test_joins(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_join");

    /* Joining on the right table's primary key: an index join. */
    check_query(db,
                "SELECT o.id, u.name FROM orders o JOIN users u ON u.id = o.user_id "
                "ORDER BY o.id",
                "10|Ada\n11|Ada\n12|Linus\n14|Margaret\n");
    /* The same join the other way round has no key to probe: a nested loop. */
    check_query(db,
                "SELECT o.id, u.name FROM users u JOIN orders o ON o.user_id = u.id "
                "ORDER BY o.id",
                "10|Ada\n11|Ada\n12|Linus\n14|Margaret\n");
    /* Extra conditions alongside the key equality, and a WHERE on top. */
    check_query(db,
                "SELECT o.id FROM orders o JOIN users u ON u.id = o.user_id AND u.active "
                "WHERE o.total > 50",
                "10\n14\n");
    /* A non-equality join condition. */
    check_query(db,
                "SELECT a.id, b.id FROM users a JOIN users b ON a.age < b.age WHERE a.id = 2",
                "2|4\n");
    /* Three tables. */
    query(db, "CREATE TABLE cities (id INT PRIMARY KEY, name TEXT);"
              "INSERT INTO cities VALUES (36, 'London'), (45, 'New York');");
    check_query(db,
                "SELECT o.id, u.name, c.name FROM orders o JOIN users u ON u.id = o.user_id "
                "JOIN cities c ON c.id = u.age",
                "10|Ada|London\n11|Ada|London\n");

    check_error(db, "SELECT * FROM users JOIN users ON users.id = users.id",
                MINIDB_ERR_UNSUPPORTED, "appears twice");
    check_error(db, "SELECT id FROM users u JOIN orders o ON o.user_id = u.id",
                MINIDB_ERR_UNKNOWN_COLUMN, "ambiguous");
    check_error(db, "SELECT * FROM users u JOIN orders o ON o.total", MINIDB_ERR_TYPE_MISMATCH,
                "ON must be a BOOL");
    close_shop(db, path);
}

/* ---- INSERT / UPDATE / DELETE -------------------------------------------- */

static void test_insert_forms_and_type_checks(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_insert");

    query(db, "INSERT INTO orders (total, id, user_id) VALUES (5, 20, 2)");
    check_query(db, "SELECT * FROM orders WHERE id = 20", "20|2|5\n");
    CHECK(minidb_last_row_count(db) == 1);

    check_error(db, "INSERT INTO users VALUES (5, 'x', 'old', TRUE)", MINIDB_ERR_TYPE_MISMATCH,
                "column 'age' is INT but the value is TEXT");
    check_error(db, "INSERT INTO users VALUES (5, 'x', 1.5, TRUE)", MINIDB_ERR_TYPE_MISMATCH,
                "INT but the value is DECIMAL");
    check_error(db, "INSERT INTO users VALUES (5, 'x', 1)", MINIDB_ERR_TYPE_MISMATCH,
                "has 4 columns but 3 values");
    check_error(db, "INSERT INTO users (id, name) VALUES (5, 'x')", MINIDB_ERR_TYPE_MISMATCH,
                "has 4 columns but 2 values");
    check_error(db, "INSERT INTO users (id, id, name, age) VALUES (5, 6, 'x', 1)",
                MINIDB_ERR_TYPE_MISMATCH, "listed twice");
    check_error(db, "INSERT INTO users VALUES (5, name, 1, TRUE)", MINIDB_ERR_UNKNOWN_COLUMN,
                "no such column 'name'");
    check_error(db, "INSERT INTO nowhere VALUES (1)", MINIDB_ERR_UNKNOWN_TABLE,
                "no such table 'nowhere'");
    check_query(db, "SELECT id FROM users", "1\n2\n3\n4\n");
    close_shop(db, path);
}

static void test_failed_multi_row_insert_inserts_nothing(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_atomic");

    check_error(db, "INSERT INTO users VALUES (5, 'a', 1, TRUE), (6, 'b', 2, TRUE), "
                    "(2, 'dup', 3, TRUE), (7, 'c', 4, TRUE)",
                MINIDB_ERR_DUPLICATE_KEY, "duplicate primary key 2");
    check_query(db, "SELECT id FROM users", "1\n2\n3\n4\n");

    /* Nor did the failed statement leave anything behind in the log. */
    CHECK_OK(minidb_close(db));
    db = open_database(path);
    check_query(db, "SELECT id FROM users", "1\n2\n3\n4\n");
    close_shop(db, path);
}

static void test_update(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_update");

    query(db, "UPDATE users SET age = age + 1, name = 'Countess' WHERE id = 1");
    CHECK(minidb_last_row_count(db) == 1);
    check_query(db, "SELECT * FROM users WHERE id = 1", "1|Countess|37|true\n");

    /* Each SET expression sees the old row, and the scan is not disturbed
     * by its own writes even when every row grows. */
    query(db, "UPDATE users SET name = 'a much, much longer name than before', age = age * 2");
    CHECK(minidb_last_row_count(db) == 4);
    check_query(db, "SELECT age FROM users", "74\n90\n56\n104\n");

    query(db, "UPDATE orders SET total = 3 WHERE user_id = 1");
    check_query(db, "SELECT total FROM orders WHERE user_id = 1", "3\n3\n");

    check_error(db, "UPDATE users SET id = 9", MINIDB_ERR_UNSUPPORTED,
                "cannot update primary key");
    check_error(db, "UPDATE users SET age = 'x'", MINIDB_ERR_TYPE_MISMATCH, "column 'age' is INT");
    check_error(db, "UPDATE users SET nope = 1", MINIDB_ERR_UNKNOWN_COLUMN, "no column 'nope'");
    check_error(db, "UPDATE users SET age = 1 / (age - 74)", MINIDB_ERR_DIVISION_BY_ZERO,
                "division by zero");
    check_query(db, "SELECT age FROM users", "74\n90\n56\n104\n");
    close_shop(db, path);
}

static void test_delete(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_delete");

    query(db, "DELETE FROM orders WHERE total < 25 AND id != 13");
    CHECK(minidb_last_row_count(db) == 2);
    check_query(db, "SELECT id FROM orders", "10\n13\n14\n");

    query(db, "DELETE FROM orders");
    check_query(db, "SELECT id FROM orders", "");
    query(db, "INSERT INTO orders VALUES (10, 1, 1)");
    check_query(db, "SELECT id FROM orders", "10\n");
    close_shop(db, path);
}

/* ---- Tables -------------------------------------------------------------- */

static void test_table_without_primary_key_keeps_insertion_order(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "sql_rowid");
    minidb_t *db = open_database(path);

    query(db, "CREATE TABLE log (message TEXT, level INT);"
              "INSERT INTO log VALUES ('boot', 1), ('warn', 2), ('boot', 1);");
    check_query(db, "SELECT message FROM log", "boot\nwarn\nboot\n");
    query(db, "DELETE FROM log WHERE level = 2");

    /* Hidden row ids keep counting up after a reopen. */
    CHECK_OK(minidb_close(db));
    db = open_database(path);
    query(db, "INSERT INTO log VALUES ('done', 3)");
    check_query(db, "SELECT message FROM log", "boot\nboot\ndone\n");
    close_shop(db, path);
}

static void test_create_and_drop_table(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_ddl");

    check_error(db, "CREATE TABLE users (id INT)", MINIDB_ERR_TABLE_EXISTS, "already exists");
    check_error(db, "DROP TABLE nowhere", MINIDB_ERR_UNKNOWN_TABLE, "no such table");
    CHECK(minidb_table_count(db) == 2);

    query(db, "DROP TABLE orders");
    CHECK(minidb_table_count(db) == 1);
    check_error(db, "SELECT * FROM orders", MINIDB_ERR_UNKNOWN_TABLE, "no such table 'orders'");

    /* The name can be reused with a different shape. */
    query(db, "CREATE TABLE orders (note TEXT); INSERT INTO orders VALUES ('fresh');");
    check_query(db, "SELECT * FROM orders", "fresh\n");

    CHECK_OK(minidb_close(db));
    db = open_database(path);
    CHECK(minidb_table_count(db) == 2);
    check_query(db, "SELECT * FROM orders", "fresh\n");
    check_query(db, "SELECT name FROM users WHERE id = 4", "Margaret\n");
    close_shop(db, path);
}

static void test_errors_stop_a_script_and_leave_earlier_statements(void) {
    char path[256];
    minidb_t *db = open_shop(path, sizeof(path), "sql_script");

    check_error(db, "DELETE FROM orders WHERE id = 10; SELEC oops; DELETE FROM orders;",
                MINIDB_ERR_SYNTAX, "expected SELECT");
    check_query(db, "SELECT id FROM orders", "11\n12\n13\n14\n");
    check_error(db, "SELECT * FROM users WHERE age", MINIDB_ERR_TYPE_MISMATCH,
                "WHERE must be a BOOL condition");
    check_error(db, "INSERT INTO users VALUES (9, 'x', 1 / 0, TRUE)",
                MINIDB_ERR_DIVISION_BY_ZERO, "division by zero");
    close_shop(db, path);
}

/* ---- Scale, access paths and durability ---------------------------------- */

static minidb_t *open_numbers(char *path, size_t capacity, const char *name, int row_count) {
    test_scratch_path(path, capacity, name);
    minidb_t *db = open_database(path);
    query(db, "CREATE TABLE numbers (id INT PRIMARY KEY, square INT, label TEXT)");
    for (int id = 0; id < row_count; id++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO numbers VALUES (%d, %d, 'n%d')", id, id * id, id);
        query(db, sql);
    }
    return db;
}

static uint64_t page_fetches(minidb_t *db) {
    minidb_storage_stats_t stats = minidb_stats(db);
    return stats.pager.cache_hits + stats.pager.cache_misses;
}

static void test_primary_key_lookups_do_not_scan(void) {
    char path[256];
    enum { ROW_COUNT = 50000 };
    minidb_t *db = open_numbers(path, sizeof(path), "sql_lookup", ROW_COUNT);

    uint64_t before = page_fetches(db);
    check_query(db, "SELECT square, label FROM numbers WHERE id = 31337",
                "982007569|n31337\n");
    uint64_t point_lookup_fetches = page_fetches(db) - before;

    before = page_fetches(db);
    check_query(db, "SELECT id FROM numbers WHERE id >= 100 AND id < 103", "100\n101\n102\n");
    uint64_t range_fetches = page_fetches(db) - before;

    before = page_fetches(db);
    check_query(db, "SELECT id FROM numbers WHERE square = 982007569", "31337\n");
    uint64_t full_scan_fetches = page_fetches(db) - before;

    /* A lookup walks root -> leaf; a scan on a non-key column reads every leaf. */
    CHECK(point_lookup_fetches <= 4);
    CHECK(range_fetches <= 4);
    CHECK(full_scan_fetches > 100);

    /* The index join does the same per outer row. */
    query(db, "CREATE TABLE picks (id INT PRIMARY KEY, number_id INT);"
              "INSERT INTO picks VALUES (1, 7), (2, 49999), (3, 123456);");
    before = page_fetches(db);
    check_query(db, "SELECT p.id, n.label FROM picks p JOIN numbers n ON n.id = p.number_id",
                "1|n7\n2|n49999\n");
    CHECK(page_fetches(db) - before <= 16);
    close_shop(db, path);
}

static void test_large_table_survives_reopen(void) {
    char path[256];
    enum { ROW_COUNT = 20000 };
    minidb_t *db = open_numbers(path, sizeof(path), "sql_reopen", ROW_COUNT);
    query(db, "UPDATE numbers SET label = 'even' WHERE id = 2 * (id / 2)");
    query(db, "DELETE FROM numbers WHERE id >= 15000");
    CHECK_OK(minidb_close(db));

    db = open_database(path);
    query(db, "SELECT id FROM numbers");
    CHECK(minidb_last_row_count(db) == 15000);
    check_query(db, "SELECT label FROM numbers WHERE id = 14998 OR id = 14999 ORDER BY id",
                "even\nn14999\n");
    close_shop(db, path);
}

static void test_synced_statements_survive_a_kill(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "sql_kill");

    fflush(stdout);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        minidb_t *db = open_database(path);
        query(db, "CREATE TABLE events (id INT PRIMARY KEY, note TEXT)");
        for (int id = 0; id < 3000; id++) {
            char sql[128];
            snprintf(sql, sizeof(sql), "INSERT INTO events VALUES (%d, 'event %d')", id, id);
            query(db, sql);
        }
        query(db, "UPDATE events SET note = 'changed' WHERE id < 10;"
                  "DELETE FROM events WHERE id >= 2990;");
        CHECK_OK(minidb_sync(db));
        kill(getpid(), SIGKILL);
    }
    int wait_status;
    CHECK(waitpid(child, &wait_status, 0) == child);
    CHECK(WIFSIGNALED(wait_status));

    minidb_t *db = open_database(path);
    CHECK(minidb_stats(db).recovered_statements > 0);
    query(db, "SELECT id FROM events");
    CHECK(minidb_last_row_count(db) == 2990);
    check_query(db, "SELECT note FROM events WHERE id = 9 OR id = 10 ORDER BY id",
                "changed\nevent 10\n");
    close_shop(db, path);
}

int main(void) {
    RUN(test_select_filter_and_projection);
    RUN(test_order_by_and_limit);
    RUN(test_joins);
    RUN(test_insert_forms_and_type_checks);
    RUN(test_failed_multi_row_insert_inserts_nothing);
    RUN(test_update);
    RUN(test_delete);
    RUN(test_table_without_primary_key_keeps_insertion_order);
    RUN(test_create_and_drop_table);
    RUN(test_errors_stop_a_script_and_leave_earlier_statements);
    RUN(test_primary_key_lookups_do_not_scan);
    RUN(test_large_table_survives_reopen);
    RUN(test_synced_statements_survive_a_kill);
    return EXIT_SUCCESS;
}
