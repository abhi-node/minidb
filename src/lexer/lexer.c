#include "minidb/lexer.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    const char *word;  /* lower case */
    minidb_token_type_t type;
} keyword_t;

static const keyword_t KEYWORDS[] = {
    {"select", MINIDB_TOKEN_SELECT},   {"from", MINIDB_TOKEN_FROM},
    {"where", MINIDB_TOKEN_WHERE},     {"and", MINIDB_TOKEN_AND},
    {"or", MINIDB_TOKEN_OR},           {"not", MINIDB_TOKEN_NOT},
    {"like", MINIDB_TOKEN_LIKE},       {"true", MINIDB_TOKEN_TRUE},
    {"false", MINIDB_TOKEN_FALSE},     {"insert", MINIDB_TOKEN_INSERT},
    {"into", MINIDB_TOKEN_INTO},       {"values", MINIDB_TOKEN_VALUES},
    {"update", MINIDB_TOKEN_UPDATE},   {"set", MINIDB_TOKEN_SET},
    {"delete", MINIDB_TOKEN_DELETE},   {"create", MINIDB_TOKEN_CREATE},
    {"drop", MINIDB_TOKEN_DROP},       {"table", MINIDB_TOKEN_TABLE},
    {"primary", MINIDB_TOKEN_PRIMARY}, {"key", MINIDB_TOKEN_KEY},
    {"join", MINIDB_TOKEN_JOIN},       {"inner", MINIDB_TOKEN_INNER},
    {"on", MINIDB_TOKEN_ON},           {"as", MINIDB_TOKEN_AS},
    {"order", MINIDB_TOKEN_ORDER},     {"by", MINIDB_TOKEN_BY},
    {"asc", MINIDB_TOKEN_ASC},         {"desc", MINIDB_TOKEN_DESC},
    {"limit", MINIDB_TOKEN_LIMIT},
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static bool is_digit(char character) { return character >= '0' && character <= '9'; }

static bool is_identifier_start(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

static bool is_identifier_part(char character) {
    return is_identifier_start(character) || is_digit(character);
}

static char to_lower(char character) {
    return character >= 'A' && character <= 'Z' ? (char)(character + ('a' - 'A')) : character;
}

/* Case-insensitive match of `text` against a lower-case keyword. */
static bool matches_keyword(const char *text, uint32_t length, const char *keyword) {
    for (uint32_t index = 0; index < length; index++) {
        if (keyword[index] == '\0' || to_lower(text[index]) != keyword[index]) {
            return false;
        }
    }
    return keyword[length] == '\0';
}

static minidb_token_type_t identifier_or_keyword(const char *text, uint32_t length) {
    for (size_t index = 0; index < KEYWORD_COUNT; index++) {
        if (matches_keyword(text, length, KEYWORDS[index].word)) {
            return KEYWORDS[index].type;
        }
    }
    return MINIDB_TOKEN_IDENTIFIER;
}

void minidb_lexer_init(minidb_lexer_t *lexer, const char *sql, size_t length) {
    lexer->cursor = sql;
    lexer->end = sql + length;
}

static void skip_whitespace_and_comments(minidb_lexer_t *lexer) {
    while (lexer->cursor < lexer->end) {
        char character = *lexer->cursor;
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            lexer->cursor++;
        } else if (character == '-' && lexer->end - lexer->cursor >= 2 &&
                   lexer->cursor[1] == '-') {
            while (lexer->cursor < lexer->end && *lexer->cursor != '\n') {
                lexer->cursor++;
            }
        } else {
            break;
        }
    }
}

/* True if the next unread character is `expected`; consumes it if so. */
static bool consume_if(minidb_lexer_t *lexer, char expected) {
    if (lexer->cursor < lexer->end && *lexer->cursor == expected) {
        lexer->cursor++;
        return true;
    }
    return false;
}

static minidb_token_type_t scan_number(minidb_lexer_t *lexer) {
    while (lexer->cursor < lexer->end && is_digit(*lexer->cursor)) {
        lexer->cursor++;
    }
    bool has_fraction = lexer->end - lexer->cursor >= 2 && lexer->cursor[0] == '.' &&
                        is_digit(lexer->cursor[1]);
    if (!has_fraction) {
        return MINIDB_TOKEN_INTEGER;
    }
    lexer->cursor++;
    while (lexer->cursor < lexer->end && is_digit(*lexer->cursor)) {
        lexer->cursor++;
    }
    return MINIDB_TOKEN_DECIMAL;
}

/* The opening quote is already consumed. A doubled quote stays in the string. */
static minidb_token_type_t scan_string(minidb_lexer_t *lexer) {
    while (lexer->cursor < lexer->end) {
        if (*lexer->cursor++ != '\'') {
            continue;
        }
        if (!consume_if(lexer, '\'')) {
            return MINIDB_TOKEN_STRING;
        }
    }
    return MINIDB_TOKEN_INVALID;  /* ran off the end without a closing quote */
}

static minidb_token_type_t scan_token(minidb_lexer_t *lexer) {
    char first = *lexer->cursor++;

    if (is_identifier_start(first)) {
        const char *start = lexer->cursor - 1;
        while (lexer->cursor < lexer->end && is_identifier_part(*lexer->cursor)) {
            lexer->cursor++;
        }
        return identifier_or_keyword(start, (uint32_t)(lexer->cursor - start));
    }
    if (is_digit(first)) {
        return scan_number(lexer);
    }

    switch (first) {
        case '\'': return scan_string(lexer);
        case ',':  return MINIDB_TOKEN_COMMA;
        case '.':  return MINIDB_TOKEN_DOT;
        case ';':  return MINIDB_TOKEN_SEMICOLON;
        case '(':  return MINIDB_TOKEN_LEFT_PAREN;
        case ')':  return MINIDB_TOKEN_RIGHT_PAREN;
        case '*':  return MINIDB_TOKEN_STAR;
        case '+':  return MINIDB_TOKEN_PLUS;
        case '-':  return MINIDB_TOKEN_MINUS;
        case '/':  return MINIDB_TOKEN_SLASH;
        case '=':  return MINIDB_TOKEN_EQUAL;
        case '!':  return consume_if(lexer, '=') ? MINIDB_TOKEN_NOT_EQUAL : MINIDB_TOKEN_INVALID;
        case '<':
            if (consume_if(lexer, '=')) {
                return MINIDB_TOKEN_LESS_EQUAL;
            }
            return consume_if(lexer, '>') ? MINIDB_TOKEN_NOT_EQUAL : MINIDB_TOKEN_LESS;
        case '>':
            return consume_if(lexer, '=') ? MINIDB_TOKEN_GREATER_EQUAL : MINIDB_TOKEN_GREATER;
        default:
            return MINIDB_TOKEN_INVALID;
    }
}

minidb_token_t minidb_lexer_next(minidb_lexer_t *lexer) {
    skip_whitespace_and_comments(lexer);

    minidb_token_t token = {MINIDB_TOKEN_END, lexer->cursor, 0};
    if (lexer->cursor < lexer->end) {
        token.type = scan_token(lexer);
        token.length = (uint32_t)(lexer->cursor - token.text);
    }
    return token;
}
