#ifndef MINIDB_LEXER_H
#define MINIDB_LEXER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Tokenizer: turns SQL text into tokens, one per call.
 *
 * Ownership: tokens are views into the SQL text and copy nothing. The text
 * must outlive every token read from it.
 */

typedef enum {
    MINIDB_TOKEN_END = 0,     /* end of input */
    MINIDB_TOKEN_INVALID,     /* stray character or unterminated string */

    MINIDB_TOKEN_IDENTIFIER,  /* table, column or type name */
    MINIDB_TOKEN_INTEGER,     /* 42 */
    MINIDB_TOKEN_DECIMAL,     /* 3.14 */
    MINIDB_TOKEN_STRING,      /* 'text', quotes included; '' escapes a quote */

    MINIDB_TOKEN_COMMA,
    MINIDB_TOKEN_DOT,
    MINIDB_TOKEN_SEMICOLON,
    MINIDB_TOKEN_LEFT_PAREN,
    MINIDB_TOKEN_RIGHT_PAREN,
    MINIDB_TOKEN_STAR,
    MINIDB_TOKEN_PLUS,
    MINIDB_TOKEN_MINUS,
    MINIDB_TOKEN_SLASH,
    MINIDB_TOKEN_EQUAL,
    MINIDB_TOKEN_NOT_EQUAL,      /* != or <> */
    MINIDB_TOKEN_LESS,
    MINIDB_TOKEN_LESS_EQUAL,
    MINIDB_TOKEN_GREATER,
    MINIDB_TOKEN_GREATER_EQUAL,

    /* Keywords, matched case-insensitively. */
    MINIDB_TOKEN_SELECT,
    MINIDB_TOKEN_FROM,
    MINIDB_TOKEN_WHERE,
    MINIDB_TOKEN_AND,
    MINIDB_TOKEN_OR,
    MINIDB_TOKEN_NOT,
    MINIDB_TOKEN_LIKE,
    MINIDB_TOKEN_TRUE,
    MINIDB_TOKEN_FALSE,
    MINIDB_TOKEN_INSERT,
    MINIDB_TOKEN_INTO,
    MINIDB_TOKEN_VALUES,
    MINIDB_TOKEN_UPDATE,
    MINIDB_TOKEN_SET,
    MINIDB_TOKEN_DELETE,
    MINIDB_TOKEN_CREATE,
    MINIDB_TOKEN_DROP,
    MINIDB_TOKEN_TABLE,
    MINIDB_TOKEN_PRIMARY,
    MINIDB_TOKEN_KEY,
    MINIDB_TOKEN_JOIN,
    MINIDB_TOKEN_INNER,
    MINIDB_TOKEN_ON,
    MINIDB_TOKEN_AS,
    MINIDB_TOKEN_ORDER,
    MINIDB_TOKEN_BY,
    MINIDB_TOKEN_ASC,
    MINIDB_TOKEN_DESC,
    MINIDB_TOKEN_LIMIT
} minidb_token_type_t;

typedef struct {
    minidb_token_type_t type;
    const char *text;  /* first character of the token, inside the SQL text */
    uint32_t length;
} minidb_token_t;

typedef struct {
    const char *cursor;  /* next unread character */
    const char *end;
} minidb_lexer_t;

void minidb_lexer_init(minidb_lexer_t *lexer, const char *sql, size_t length);

/* Skips whitespace and `--` comments, then returns the next token. */
minidb_token_t minidb_lexer_next(minidb_lexer_t *lexer);

#endif
