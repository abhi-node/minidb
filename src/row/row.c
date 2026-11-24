#include "minidb/row.h"

#include <string.h>

/*
 * Multi-byte fields are copied with memcpy in host byte order. minidb only
 * targets little-endian machines, so host order is the on-disk order.
 */

#define INT_SIZE     sizeof(int64_t)
#define DECIMAL_SIZE sizeof(double)
#define BOOL_SIZE    1u
#define TEXT_LENGTH_SIZE sizeof(uint16_t)

size_t minidb_row_encoded_size(const minidb_schema_t *schema, const minidb_value_t *values) {
    size_t size = 0;
    for (uint32_t index = 0; index < schema->column_count; index++) {
        switch (schema->columns[index].type) {
            case MINIDB_TYPE_INT:     size += INT_SIZE; break;
            case MINIDB_TYPE_DECIMAL: size += DECIMAL_SIZE; break;
            case MINIDB_TYPE_BOOL:    size += BOOL_SIZE; break;
            case MINIDB_TYPE_TEXT:
                size += TEXT_LENGTH_SIZE + values[index].as.text.length;
                break;
        }
    }
    return size;
}

void minidb_row_encode(const minidb_schema_t *schema, const minidb_value_t *values,
                       uint8_t *out_bytes) {
    uint8_t *cursor = out_bytes;
    for (uint32_t index = 0; index < schema->column_count; index++) {
        const minidb_value_t *value = &values[index];
        switch (schema->columns[index].type) {
            case MINIDB_TYPE_INT:
                memcpy(cursor, &value->as.integer, INT_SIZE);
                cursor += INT_SIZE;
                break;
            case MINIDB_TYPE_DECIMAL:
                memcpy(cursor, &value->as.decimal, DECIMAL_SIZE);
                cursor += DECIMAL_SIZE;
                break;
            case MINIDB_TYPE_BOOL:
                *cursor++ = value->as.boolean ? 1 : 0;
                break;
            case MINIDB_TYPE_TEXT: {
                uint16_t text_length = (uint16_t)value->as.text.length;
                memcpy(cursor, &text_length, TEXT_LENGTH_SIZE);
                cursor += TEXT_LENGTH_SIZE;
                memcpy(cursor, value->as.text.bytes, text_length);
                cursor += text_length;
                break;
            }
        }
    }
}

minidb_status_t minidb_row_decode(const minidb_schema_t *schema, const uint8_t *bytes,
                                  size_t length, minidb_value_t *out_values) {
    const uint8_t *cursor = bytes;
    size_t remaining = length;

    for (uint32_t index = 0; index < schema->column_count; index++) {
        minidb_value_t *value = &out_values[index];
        value->type = schema->columns[index].type;

        switch (value->type) {
            case MINIDB_TYPE_INT:
                if (remaining < INT_SIZE) {
                    return MINIDB_ERR_CORRUPT;
                }
                memcpy(&value->as.integer, cursor, INT_SIZE);
                cursor += INT_SIZE;
                remaining -= INT_SIZE;
                break;
            case MINIDB_TYPE_DECIMAL:
                if (remaining < DECIMAL_SIZE) {
                    return MINIDB_ERR_CORRUPT;
                }
                memcpy(&value->as.decimal, cursor, DECIMAL_SIZE);
                cursor += DECIMAL_SIZE;
                remaining -= DECIMAL_SIZE;
                break;
            case MINIDB_TYPE_BOOL:
                if (remaining < BOOL_SIZE) {
                    return MINIDB_ERR_CORRUPT;
                }
                value->as.boolean = *cursor != 0;
                cursor += BOOL_SIZE;
                remaining -= BOOL_SIZE;
                break;
            case MINIDB_TYPE_TEXT: {
                uint16_t text_length;
                if (remaining < TEXT_LENGTH_SIZE) {
                    return MINIDB_ERR_CORRUPT;
                }
                memcpy(&text_length, cursor, TEXT_LENGTH_SIZE);
                cursor += TEXT_LENGTH_SIZE;
                remaining -= TEXT_LENGTH_SIZE;
                if (remaining < text_length) {
                    return MINIDB_ERR_CORRUPT;
                }
                value->as.text.bytes = (const char *)cursor;
                value->as.text.length = text_length;
                cursor += text_length;
                remaining -= text_length;
                break;
            }
        }
    }
    return remaining == 0 ? MINIDB_OK : MINIDB_ERR_CORRUPT;
}
