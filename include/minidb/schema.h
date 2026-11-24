#ifndef MINIDB_SCHEMA_H
#define MINIDB_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#include "minidb/status.h"
#include "minidb/value.h"

/*
 * A table definition: its name, ordered columns and primary key.
 *
 * Ownership: a schema is a plain fixed-size value with no pointers, so it
 * can be copied, embedded and compared freely.
 */

#define MINIDB_MAX_COLUMNS     32
#define MINIDB_MAX_NAME_LENGTH 63

/* primary_key_index of a table with no declared key (it gets a hidden row id). */
#define MINIDB_NO_PRIMARY_KEY (-1)

typedef struct {
    char name[MINIDB_MAX_NAME_LENGTH + 1];
    minidb_type_t type;
} minidb_column_t;

typedef struct {
    char table_name[MINIDB_MAX_NAME_LENGTH + 1];
    uint32_t column_count;
    int32_t primary_key_index;  /* index into columns, or MINIDB_NO_PRIMARY_KEY */
    minidb_column_t columns[MINIDB_MAX_COLUMNS];
} minidb_schema_t;

/* Starts an empty schema. Fails if the table name is empty or too long. */
minidb_status_t minidb_schema_init(minidb_schema_t *schema, const char *table_name);

/*
 * Appends a column. Fails on a duplicate name, a full schema, a second
 * primary key, or a primary key that is not an INT.
 */
minidb_status_t minidb_schema_add_column(minidb_schema_t *schema, const char *name,
                                         minidb_type_t type, bool is_primary_key);

/* Index of the named column, or -1 when the table has no such column. */
int32_t minidb_schema_find_column(const minidb_schema_t *schema, const char *name);

/*
 * Compact binary form used by the catalog and the write-ahead log:
 *
 *   u8 name_length, name bytes, u8 column_count, i8 primary_key_index,
 *   then per column: u8 type, u8 name_length, name bytes.
 */
size_t minidb_schema_encoded_size(const minidb_schema_t *schema);
void minidb_schema_encode(const minidb_schema_t *schema, uint8_t *out_bytes);
minidb_status_t minidb_schema_decode(const uint8_t *bytes, size_t length,
                                     minidb_schema_t *out_schema);

#endif
