/*
 * minidb command-line shell.
 *
 *   minidb <database file>             interactive prompt
 *   minidb <database file> < script    run statements from standard input
 *
 * Statements end with ';' and may span lines. Lines starting with '.' are
 * shell commands (see .help).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "minidb/minidb.h"

/* ---- Printing results ---------------------------------------------------- */

typedef struct {
    bool has_printed_header;
} result_printer_t;

static bool print_row(void *context, const minidb_row_t *row) {
    result_printer_t *printer = context;
    if (!printer->has_printed_header) {
        for (uint32_t column = 0; column < row->column_count; column++) {
            printf("%s%s", column > 0 ? " | " : "", row->column_names[column]);
        }
        putchar('\n');
        printer->has_printed_header = true;
    }
    for (uint32_t column = 0; column < row->column_count; column++) {
        if (column > 0) {
            fputs(" | ", stdout);
        }
        minidb_value_print(&row->values[column], stdout);
    }
    putchar('\n');
    return true;
}

static void run_sql(minidb_t *db, const char *sql) {
    result_printer_t printer = {false};
    if (minidb_exec(db, sql, print_row, &printer) != MINIDB_OK) {
        fprintf(stderr, "error: %s\n", minidb_error_message(db));
    }
}

/* ---- Dot commands -------------------------------------------------------- */

static void print_schema(const minidb_schema_t *schema) {
    printf("%s (", schema->table_name);
    for (uint32_t column = 0; column < schema->column_count; column++) {
        printf("%s%s %s%s", column > 0 ? ", " : "", schema->columns[column].name,
               minidb_type_name(schema->columns[column].type),
               (int32_t)column == schema->primary_key_index ? " PRIMARY KEY" : "");
    }
    puts(")");
}

static void print_stats(const minidb_t *db) {
    minidb_storage_stats_t stats = minidb_stats(db);
    printf("log:   %" PRIu64 " commits, %" PRIu64 " fsyncs, %" PRIu64 " bytes written\n",
           stats.wal.commits, stats.wal.fsyncs, stats.wal.bytes_written);
    printf("cache: %" PRIu64 " hits, %" PRIu64 " misses, %" PRIu64 " pages written\n",
           stats.pager.cache_hits, stats.pager.cache_misses, stats.pager.pages_written);
    printf("checkpoints: %" PRIu64 ", statements recovered at open: %" PRIu64 "\n",
           stats.checkpoints, stats.recovered_statements);
}

/* Returns false when the shell should exit. */
static bool run_dot_command(minidb_t *db, const char *command) {
    if (strcmp(command, ".exit") == 0 || strcmp(command, ".quit") == 0) {
        return false;
    }
    if (strcmp(command, ".tables") == 0) {
        for (uint32_t index = 0; index < minidb_table_count(db); index++) {
            print_schema(minidb_table_schema(db, index));
        }
    } else if (strcmp(command, ".stats") == 0) {
        print_stats(db);
    } else if (strcmp(command, ".checkpoint") == 0) {
        if (minidb_checkpoint(db) != MINIDB_OK) {
            fprintf(stderr, "error: checkpoint failed\n");
        }
    } else {
        puts(".tables       list tables and their columns\n"
             ".stats        show log, cache and checkpoint counters\n"
             ".checkpoint   write all changes into the database file\n"
             ".exit         leave the shell");
    }
    return true;
}

/* ---- Read loop ----------------------------------------------------------- */

/* True if the text ends a statement: its last non-blank character is ';'. */
static bool ends_with_semicolon(const char *text, size_t length) {
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\n' || text[length - 1] == '\r')) {
        length--;
    }
    return length > 0 && text[length - 1] == ';';
}

static void read_loop(minidb_t *db, bool is_interactive) {
    char line[4096];
    char *statement = NULL;  /* lines gathered so far for the current statement */
    size_t statement_length = 0;

    for (;;) {
        if (is_interactive) {
            fputs(statement_length == 0 ? "minidb> " : "   ...> ", stdout);
            fflush(stdout);
        }
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        size_t line_length = strlen(line);

        if (statement_length == 0 && line[0] == '.') {
            line[strcspn(line, "\r\n")] = '\0';
            if (!run_dot_command(db, line)) {
                break;
            }
            continue;
        }

        char *grown = realloc(statement, statement_length + line_length + 1);
        if (grown == NULL) {
            fprintf(stderr, "error: out of memory\n");
            break;
        }
        statement = grown;
        memcpy(statement + statement_length, line, line_length + 1);
        statement_length += line_length;

        if (ends_with_semicolon(statement, statement_length)) {
            run_sql(db, statement);
            statement_length = 0;
            /* Someone is waiting on the prompt: do not leave their commit
             * sitting in the group-commit buffer. */
            if (is_interactive) {
                minidb_sync(db);
            }
        }
    }

    if (statement_length > 0) {
        run_sql(db, statement);  /* a final statement without its ';' */
    }
    free(statement);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <database file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    minidb_t *db;
    minidb_status_t status = minidb_open(argv[1], NULL, &db);
    if (status != MINIDB_OK) {
        fprintf(stderr, "cannot open '%s': %s\n", argv[1], minidb_status_name(status));
        return EXIT_FAILURE;
    }

    read_loop(db, isatty(STDIN_FILENO) != 0);

    if (minidb_close(db) != MINIDB_OK) {
        fprintf(stderr, "error: could not write the database on close\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
