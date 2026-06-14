#ifndef MINIDB_LEXER_H
#define MINIDB_LEXER_H

#include <stddef.h>
#include "minidb/schema.h"
#include <stdio.h>

typedef enum {
    SELECT_TOKEN,
    FROM_TOKEN,
    WHERE_TOKEN,
    AND_TOKEN,
    OR_TOKEN,
    GREATER_TOKEN,
    LESS_TOKEN,
    EQUAL_TOKEN,
    PLUS_TOKEN,
    MINUS_TOKEN,
    ASTERIK_TOKEN,
    DIVIDE_TOKEN,
    STRING_TOKEN,
    TERMINAL_TOKEN,
    COLUMN_TOKEN
} minidb_token_t;

typedef struct {
    minidb_token_t *tokens;
    size_t n_tokens;
    size_t capacity;
} minidb_token_set_t;

minidb_db_status tokenize(FILE *f, minidb_token_set_t *out_tokens);

#endif
