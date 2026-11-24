#ifndef MINIDB_VALUE_H
#define MINIDB_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * A single typed SQL value, as it flows between operators.
 *
 * Ownership: a value never owns memory. TEXT values borrow their bytes from
 * whoever produced them (a pinned page, the SQL text, an operator's buffer)
 * and are only valid for as long as that producer says they are.
 */

typedef enum {
    MINIDB_TYPE_INT = 0,   /* 64-bit signed integer */
    MINIDB_TYPE_DECIMAL,   /* IEEE-754 double */
    MINIDB_TYPE_TEXT,      /* byte string, not NUL-terminated */
    MINIDB_TYPE_BOOL
} minidb_type_t;

typedef struct {
    minidb_type_t type;
    union {
        int64_t integer;
        double decimal;
        bool boolean;
        struct {
            const char *bytes;  /* borrowed, not NUL-terminated */
            uint32_t length;
        } text;
    } as;
} minidb_value_t;

static inline minidb_value_t minidb_value_int(int64_t integer) {
    minidb_value_t value = {.type = MINIDB_TYPE_INT};
    value.as.integer = integer;
    return value;
}

static inline minidb_value_t minidb_value_decimal(double decimal) {
    minidb_value_t value = {.type = MINIDB_TYPE_DECIMAL};
    value.as.decimal = decimal;
    return value;
}

static inline minidb_value_t minidb_value_bool(bool boolean) {
    minidb_value_t value = {.type = MINIDB_TYPE_BOOL};
    value.as.boolean = boolean;
    return value;
}

static inline minidb_value_t minidb_value_text(const char *bytes, uint32_t length) {
    minidb_value_t value = {.type = MINIDB_TYPE_TEXT};
    value.as.text.bytes = bytes;
    value.as.text.length = length;
    return value;
}

static inline bool minidb_type_is_numeric(minidb_type_t type) {
    return type == MINIDB_TYPE_INT || type == MINIDB_TYPE_DECIMAL;
}

/* SQL spelling of a type: "INT", "DECIMAL", "TEXT" or "BOOL". */
const char *minidb_type_name(minidb_type_t type);

/* True when values of the two types can be ordered against each other. */
bool minidb_types_comparable(minidb_type_t left, minidb_type_t right);

/*
 * Three-way comparison: negative, zero or positive. The types must be
 * comparable; INT and DECIMAL compare numerically, TEXT bytewise.
 */
int minidb_value_compare(const minidb_value_t *left, const minidb_value_t *right);

/* Writes the value the way the CLI displays it. */
void minidb_value_print(const minidb_value_t *value, FILE *stream);

#endif
