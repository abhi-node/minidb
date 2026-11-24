#ifndef MINIDB_TEST_H
#define MINIDB_TEST_H

/*
 * Minimal test harness: CHECK aborts the test binary on the first failure,
 * RUN prints the test name so a failing ctest log shows where it stopped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "minidb/status.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

#define CHECK_STATUS(expected, expression)                                    \
    do {                                                                      \
        minidb_status_t check_status_ = (expression);                         \
        if (check_status_ != (expected)) {                                    \
            fprintf(stderr, "%s:%d: %s returned \"%s\", expected \"%s\"\n",   \
                    __FILE__, __LINE__, #expression,                          \
                    minidb_status_name(check_status_),                        \
                    minidb_status_name(expected));                            \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

#define CHECK_OK(expression) CHECK_STATUS(MINIDB_OK, expression)

#define RUN(test_function)                 \
    do {                                   \
        printf("  %s\n", #test_function);  \
        test_function();                   \
    } while (0)

/*
 * Builds a per-process scratch path under /tmp and removes any database or
 * WAL file left there by an earlier run.
 */
static inline void test_scratch_path(char *out_path, size_t capacity, const char *name) {
    char wal_path[512];
    snprintf(out_path, capacity, "/tmp/minidb_test_%s_%ld.db", name, (long)getpid());
    snprintf(wal_path, sizeof(wal_path), "%s.wal", out_path);
    unlink(out_path);
    unlink(wal_path);
}

static inline void test_remove_database(const char *path) {
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    unlink(path);
    unlink(wal_path);
}

#endif
