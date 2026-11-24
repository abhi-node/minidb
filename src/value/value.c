#include "minidb/value.h"

#include <inttypes.h>
#include <string.h>

const char *minidb_type_name(minidb_type_t type) {
    switch (type) {
        case MINIDB_TYPE_INT:     return "INT";
        case MINIDB_TYPE_DECIMAL: return "DECIMAL";
        case MINIDB_TYPE_TEXT:    return "TEXT";
        case MINIDB_TYPE_BOOL:    return "BOOL";
    }
    return "UNKNOWN";
}

bool minidb_types_comparable(minidb_type_t left, minidb_type_t right) {
    if (minidb_type_is_numeric(left) && minidb_type_is_numeric(right)) {
        return true;
    }
    return left == right;
}

static double value_as_double(const minidb_value_t *value) {
    return value->type == MINIDB_TYPE_INT ? (double)value->as.integer : value->as.decimal;
}

static int compare_text(const minidb_value_t *left, const minidb_value_t *right) {
    uint32_t left_length = left->as.text.length;
    uint32_t right_length = right->as.text.length;
    uint32_t shared_length = left_length < right_length ? left_length : right_length;

    int order = memcmp(left->as.text.bytes, right->as.text.bytes, shared_length);
    if (order != 0) {
        return order;
    }
    return (left_length > right_length) - (left_length < right_length);
}

int minidb_value_compare(const minidb_value_t *left, const minidb_value_t *right) {
    if (left->type == MINIDB_TYPE_INT && right->type == MINIDB_TYPE_INT) {
        return (left->as.integer > right->as.integer) - (left->as.integer < right->as.integer);
    }
    if (minidb_type_is_numeric(left->type)) {
        double left_number = value_as_double(left);
        double right_number = value_as_double(right);
        return (left_number > right_number) - (left_number < right_number);
    }
    if (left->type == MINIDB_TYPE_TEXT) {
        return compare_text(left, right);
    }
    return (int)left->as.boolean - (int)right->as.boolean;
}

void minidb_value_print(const minidb_value_t *value, FILE *stream) {
    switch (value->type) {
        case MINIDB_TYPE_INT:
            fprintf(stream, "%" PRId64, value->as.integer);
            break;
        case MINIDB_TYPE_DECIMAL:
            fprintf(stream, "%g", value->as.decimal);
            break;
        case MINIDB_TYPE_TEXT:
            fwrite(value->as.text.bytes, 1, value->as.text.length, stream);
            break;
        case MINIDB_TYPE_BOOL:
            fputs(value->as.boolean ? "true" : "false", stream);
            break;
    }
}
