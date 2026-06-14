#include "minidb/lexer.h"
#include "minidb/schema.h"
#include <stdlib.h>
#include <string.h>

#define TOKEN_MAX 1024
#define TOKEN_INIT_CAP 16
#define TOKEN_INCREASE_FACTOR 2

minidb_db_status insert_token(minidb_token_t t, minidb_token_set_t *out_tokens) {
    if (out_tokens->capacity == out_tokens->n_tokens) {
        size_t new_capacity = out_tokens->capacity*TOKEN_INCREASE_FACTOR;
        minidb_token_t *tokens = realloc(out_tokens->tokens, new_capacity * sizeof(minidb_token_t));
        if (tokens == NULL) {
            return MINIDB_ERR_MEM_ALLOC_FAIL;
        }

        out_tokens->tokens = tokens;
        out_tokens->capacity = new_capacity;
    }

    out_tokens->tokens[out_tokens->n_tokens] = t;
    out_tokens->n_tokens++;

    return MINIDB_OK;
}

minidb_token_t match_token(char *buffer, size_t size) {
    if (strcmp(buffer, "SELECT") == 0) {
        return SELECT_TOKEN;
    } else if (strcmp(buffer, "FROM") == 0) {
        return FROM_TOKEN;
    } else if (strcmp(buffer, "WHERE") == 0) {
        return WHERE_TOKEN;
    } else if (strcmp(buffer, ";") == 0) {
        return TERMINAL_TOKEN;
    } else if (strcmp(buffer, "AND") == 0) {
        return AND_TOKEN;
    } else if (strcmp(buffer, "OR") == 0) {
        return OR_TOKEN;
    } else if (strcmp(buffer, "+") == 0) {
        return PLUS_TOKEN;
    } else if (strcmp(buffer, "-") == 0) {
        return MINUS_TOKEN;
    } else if (strcmp(buffer, "/") == 0) {
        return DIVIDE_TOKEN;
    } else if (strcmp(buffer, "*") == 0) {
        return ASTERIK_TOKEN;
    } else if (strcmp(buffer, "=") == 0) {
        return EQUAL_TOKEN;
    } else if (strcmp(buffer, ">") == 0) {
        return GREATER_TOKEN;
    } else if (strcmp(buffer, "<") == 0) {
        return LESS_TOKEN;
    }

    if (buffer[0] == '\'' && buffer[size] == '\'') {
        return STRING_TOKEN;
    }

    return COLUMN_TOKEN;
}

minidb_db_status tokenize(FILE *f, minidb_token_set_t *out_tokens) {
    if (out_tokens == NULL || f == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }

    out_tokens->n_tokens = 0;
    out_tokens->capacity =  TOKEN_INIT_CAP;
    out_tokens->tokens = malloc(sizeof(minidb_token_t)*TOKEN_INIT_CAP);

    if (out_tokens->tokens == NULL) {
        return MINIDB_ERR_MEM_ALLOC_FAIL;
    }

    int c;
    char buffer[TOKEN_MAX+1];
    size_t i = 0;
    while ((c = fgetc(f)) != EOF) {
        if (i > TOKEN_MAX) {
            return MINIDB_ERR_INVALID_STATEMENT;
        }
        if (c == ' ' || c == '\n') {
            buffer[i] = '\0';
            minidb_token_t token = match_token(buffer, i);
            insert_token(token, out_tokens);
            i = 0;
        }
        buffer[i] = c;
        i++;
    }

    return MINIDB_OK;
}


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
