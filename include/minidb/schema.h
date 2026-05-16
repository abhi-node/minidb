#ifndef SCHEMA_UTILS_H
#define SCHEMA_UTILS_H

#include <cstdint>
#include <cstddef>

typedef enum {
    COLUMN_INT = 0,
    COLUMN_DECIMAL
} ColumnType;

typedef struct {
    char *name;
    ColumnType type;
} Column;

typedef struct {
    uint32_t size;
    Column *cols;
} Schema;

typedef struct {
    char *bytes;
} Tuple;

typedef struct {
    Schema schema;
    Tuple *rows;
    size_t n_rows;
    size_t capacity;
} Table;

void create_table();

#endif
