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
    STRING_TOKEN
} minidb_token_t;

typedef struct {
    minidb_token_t *tokens;
    size_t length;
} minidb_token_set_t;

minidb_db_status tokenize(FILE *f, minidb_token_set_t *out_tokens);



/*
 * Goal - produce a lexer capable of capturing each part of the expressions
 * SELECT * FROM users;
 SELECT name, email FROM users;
 SELECT name, email FROM users WHERE age > 18;
 SELECT * FROM users WHERE name = 'Alice' AND active = 1;
 SELECT * FROM users WHERE email LIKE '%@gmail.com';
 SELECT * FROM users ORDER BY created_at DESC LIMIT 10;

 Token types identified:
 SELECT
 FROM
 WHERE
 AND
 OR

 <, >, =
 +, -, /, * (how to represent this as a multiplication and a wildcard? Makes parsing more complex, we can generalize to an ASTERIK token
 'string'

 column token represents variables - we can look into refining this on joins
(we will handle order by and desc and limit later.)

support other statements after easily.
How to invalidate malformed statements
- no matching column
- no matching table
- unterminated statement

If the statement ends without a semicolon, we can invalidate it.
If a column does not match, invalidate
If no matching table, invalidate

clear concept is we just have to handle most of this at parsing, except the semicolon finalizer

return a vector of tokens
- should it be a struct - it's likely easiest to represent that way
 */

#endif
