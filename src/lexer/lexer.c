#include "minidb/lexer.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    const char *word;  /* lower case */
    uint32_t length;
    minidb_token_type_t type;
} keyword_t;

#define KEYWORD(word, type) {word, sizeof(word) - 1, type}

static const keyword_t KEYWORDS[] = {
    KEYWORD("select", MINIDB_TOKEN_SELECT),   KEYWORD("from", MINIDB_TOKEN_FROM),
    KEYWORD("where", MINIDB_TOKEN_WHERE),     KEYWORD("and", MINIDB_TOKEN_AND),
    KEYWORD("or", MINIDB_TOKEN_OR),           KEYWORD("not", MINIDB_TOKEN_NOT),
    KEYWORD("like", MINIDB_TOKEN_LIKE),       KEYWORD("true", MINIDB_TOKEN_TRUE),
    KEYWORD("false", MINIDB_TOKEN_FALSE),     KEYWORD("insert", MINIDB_TOKEN_INSERT),
    KEYWORD("into", MINIDB_TOKEN_INTO),       KEYWORD("values", MINIDB_TOKEN_VALUES),
    KEYWORD("update", MINIDB_TOKEN_UPDATE),   KEYWORD("set", MINIDB_TOKEN_SET),
    KEYWORD("delete", MINIDB_TOKEN_DELETE),   KEYWORD("create", MINIDB_TOKEN_CREATE),
    KEYWORD("drop", MINIDB_TOKEN_DROP),       KEYWORD("table", MINIDB_TOKEN_TABLE),
    KEYWORD("primary", MINIDB_TOKEN_PRIMARY), KEYWORD("key", MINIDB_TOKEN_KEY),
    KEYWORD("join", MINIDB_TOKEN_JOIN),       KEYWORD("inner", MINIDB_TOKEN_INNER),
    KEYWORD("on", MINIDB_TOKEN_ON),           KEYWORD("as", MINIDB_TOKEN_AS),
    KEYWORD("order", MINIDB_TOKEN_ORDER),     KEYWORD("by", MINIDB_TOKEN_BY),
    KEYWORD("asc", MINIDB_TOKEN_ASC),         KEYWORD("desc", MINIDB_TOKEN_DESC),
    KEYWORD("limit", MINIDB_TOKEN_LIMIT),
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))
#define LONGEST_KEYWORD 7

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

/* Case-insensitive match of `length` characters against a lower-case keyword. */
static bool matches_keyword(const char *text, uint32_t length, const char *keyword) {
    for (uint32_t index = 0; index < length; index++) {
        if (to_lower(text[index]) != keyword[index]) {
            return false;
        }
    }
    return true;
}

static minidb_token_type_t identifier_or_keyword(const char *text, uint32_t length) {
    if (length > LONGEST_KEYWORD) {
        return MINIDB_TOKEN_IDENTIFIER;
    }
    char first = to_lower(text[0]);
    for (size_t index = 0; index < KEYWORD_COUNT; index++) {
        /* Length and first letter rule out almost every keyword before the
         * character-by-character comparison. */
        const keyword_t *keyword = &KEYWORDS[index];
        if (keyword->length == length && keyword->word[0] == first &&
            matches_keyword(text, length, keyword->word)) {
            return keyword->type;
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
