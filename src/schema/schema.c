#include "minidb/schema.h"

#include <string.h>

minidb_status_t minidb_schema_init(minidb_schema_t *schema, const char *table_name) {
    size_t name_length = strlen(table_name);
    if (name_length == 0 || name_length > MINIDB_MAX_NAME_LENGTH) {
        return MINIDB_ERR_INVALID_ARGUMENT;
    }
    memset(schema, 0, sizeof(*schema));
    memcpy(schema->table_name, table_name, name_length);
    schema->primary_key_index = MINIDB_NO_PRIMARY_KEY;
    return MINIDB_OK;
}

minidb_status_t minidb_schema_add_column(minidb_schema_t *schema, const char *name,
                                         minidb_type_t type, bool is_primary_key) {
    size_t name_length = strlen(name);
    if (name_length == 0 || name_length > MINIDB_MAX_NAME_LENGTH) {
        return MINIDB_ERR_INVALID_ARGUMENT;
    }
    if (schema->column_count == MINIDB_MAX_COLUMNS) {
        return MINIDB_ERR_UNSUPPORTED;
    }
    if (minidb_schema_find_column(schema, name) >= 0) {
        return MINIDB_ERR_INVALID_ARGUMENT;
    }
    if (is_primary_key) {
        if (schema->primary_key_index != MINIDB_NO_PRIMARY_KEY) {
            return MINIDB_ERR_UNSUPPORTED;
        }
        if (type != MINIDB_TYPE_INT) {
            return MINIDB_ERR_TYPE_MISMATCH;
        }
        schema->primary_key_index = (int32_t)schema->column_count;
    }

    minidb_column_t *column = &schema->columns[schema->column_count++];
    memcpy(column->name, name, name_length + 1);
    column->type = type;
    return MINIDB_OK;
}

int32_t minidb_schema_find_column(const minidb_schema_t *schema, const char *name) {
    for (uint32_t index = 0; index < schema->column_count; index++) {
        if (strcmp(schema->columns[index].name, name) == 0) {
            return (int32_t)index;
        }
    }
    return -1;
}

size_t minidb_schema_encoded_size(const minidb_schema_t *schema) {
    size_t size = 1 + strlen(schema->table_name) + 1 + 1;
    for (uint32_t index = 0; index < schema->column_count; index++) {
        size += 1 + 1 + strlen(schema->columns[index].name);
    }
    return size;
}

void minidb_schema_encode(const minidb_schema_t *schema, uint8_t *out_bytes) {
    uint8_t *cursor = out_bytes;

    size_t table_name_length = strlen(schema->table_name);
    *cursor++ = (uint8_t)table_name_length;
    memcpy(cursor, schema->table_name, table_name_length);
    cursor += table_name_length;

    *cursor++ = (uint8_t)schema->column_count;
    *cursor++ = (uint8_t)(int8_t)schema->primary_key_index;

    for (uint32_t index = 0; index < schema->column_count; index++) {
        const minidb_column_t *column = &schema->columns[index];
        size_t column_name_length = strlen(column->name);
        *cursor++ = (uint8_t)column->type;
        *cursor++ = (uint8_t)column_name_length;
        memcpy(cursor, column->name, column_name_length);
        cursor += column_name_length;
    }
}

/* Reads a length-prefixed name; advances *cursor. Returns false on overrun. */
static bool decode_name(const uint8_t **cursor, const uint8_t *end, char *out_name) {
    if (*cursor >= end) {
        return false;
    }
    size_t name_length = *(*cursor)++;
    if (name_length == 0 || name_length > MINIDB_MAX_NAME_LENGTH ||
        (size_t)(end - *cursor) < name_length) {
        return false;
    }
    memcpy(out_name, *cursor, name_length);
    out_name[name_length] = '\0';
    *cursor += name_length;
    return true;
}

minidb_status_t minidb_schema_decode(const uint8_t *bytes, size_t length,
                                     minidb_schema_t *out_schema) {
    const uint8_t *cursor = bytes;
    const uint8_t *end = bytes + length;

    memset(out_schema, 0, sizeof(*out_schema));
    if (!decode_name(&cursor, end, out_schema->table_name) || end - cursor < 2) {
        return MINIDB_ERR_CORRUPT;
    }

    uint32_t column_count = *cursor++;
    int32_t primary_key_index = (int8_t)*cursor++;
    if (column_count > MINIDB_MAX_COLUMNS || primary_key_index < MINIDB_NO_PRIMARY_KEY ||
        primary_key_index >= (int32_t)column_count) {
        return MINIDB_ERR_CORRUPT;
    }
    out_schema->column_count = column_count;
    out_schema->primary_key_index = primary_key_index;

    for (uint32_t index = 0; index < column_count; index++) {
        minidb_column_t *column = &out_schema->columns[index];
        if (cursor >= end || *cursor > MINIDB_TYPE_BOOL) {
            return MINIDB_ERR_CORRUPT;
        }
        column->type = (minidb_type_t)*cursor++;
        if (!decode_name(&cursor, end, column->name)) {
            return MINIDB_ERR_CORRUPT;
        }
    }
    return cursor == end ? MINIDB_OK : MINIDB_ERR_CORRUPT;
}
