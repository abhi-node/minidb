#include "test.h"

#include "minidb/lexer.h"

/* Tokenizes `sql` and checks the token types, which must end with END. */
static void check_tokens(const char *sql, const minidb_token_type_t *expected) {
    minidb_lexer_t lexer;
    minidb_lexer_init(&lexer, sql, strlen(sql));
    for (size_t index = 0;; index++) {
        minidb_token_t token = minidb_lexer_next(&lexer);
        if (token.type != expected[index]) {
            fprintf(stderr, "token %zu of \"%s\": got %d, expected %d\n", index, sql,
                    (int)token.type, (int)expected[index]);
            exit(EXIT_FAILURE);
        }
        if (token.type == MINIDB_TOKEN_END) {
            return;
        }
    }
}

static void test_select_statement(void) {
    const minidb_token_type_t expected[] = {
        MINIDB_TOKEN_SELECT,     MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_COMMA,
        MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_FROM,       MINIDB_TOKEN_IDENTIFIER,
        MINIDB_TOKEN_WHERE,      MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_GREATER_EQUAL,
        MINIDB_TOKEN_INTEGER,    MINIDB_TOKEN_AND,        MINIDB_TOKEN_IDENTIFIER,
        MINIDB_TOKEN_EQUAL,      MINIDB_TOKEN_STRING,     MINIDB_TOKEN_SEMICOLON,
        MINIDB_TOKEN_END,
    };
    check_tokens("SELECT name, email FROM users WHERE age >= 18 AND name = 'Alice';", expected);
}

static void test_keywords_ignore_case_and_identifiers_do_not_match_prefixes(void) {
    const minidb_token_type_t expected[] = {
        MINIDB_TOKEN_SELECT,     MINIDB_TOKEN_FROM,       MINIDB_TOKEN_IDENTIFIER,
        MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_END,
    };
    check_tokens("sElEcT from selected fromage _on", expected);
}

static void test_operators_and_numbers(void) {
    const minidb_token_type_t expected[] = {
        MINIDB_TOKEN_INTEGER,   MINIDB_TOKEN_PLUS,      MINIDB_TOKEN_DECIMAL,
        MINIDB_TOKEN_STAR,      MINIDB_TOKEN_LEFT_PAREN, MINIDB_TOKEN_IDENTIFIER,
        MINIDB_TOKEN_DOT,       MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_MINUS,
        MINIDB_TOKEN_INTEGER,   MINIDB_TOKEN_RIGHT_PAREN, MINIDB_TOKEN_SLASH,
        MINIDB_TOKEN_INTEGER,   MINIDB_TOKEN_NOT_EQUAL, MINIDB_TOKEN_NOT_EQUAL,
        MINIDB_TOKEN_LESS,      MINIDB_TOKEN_LESS_EQUAL, MINIDB_TOKEN_GREATER,
        MINIDB_TOKEN_END,
    };
    check_tokens("1 + 2.5 * (t.x - 3) / 4 != <> < <= >", expected);
}

static void test_strings_and_comments(void) {
    const char *sql = "'it''s' -- trailing comment\n  'two'";
    minidb_lexer_t lexer;
    minidb_lexer_init(&lexer, sql, strlen(sql));

    minidb_token_t first = minidb_lexer_next(&lexer);
    CHECK(first.type == MINIDB_TOKEN_STRING && first.length == 7);
    CHECK(memcmp(first.text, "'it''s'", 7) == 0);

    minidb_token_t second = minidb_lexer_next(&lexer);
    CHECK(second.type == MINIDB_TOKEN_STRING && second.length == 5);
    CHECK(minidb_lexer_next(&lexer).type == MINIDB_TOKEN_END);
}

static void test_invalid_input(void) {
    const minidb_token_type_t unterminated[] = {MINIDB_TOKEN_INVALID, MINIDB_TOKEN_END};
    check_tokens("'never closed", unterminated);

    const minidb_token_type_t stray[] = {MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_INVALID,
                                         MINIDB_TOKEN_IDENTIFIER, MINIDB_TOKEN_END};
    check_tokens("a @ b", stray);
}

int main(void) {
    RUN(test_select_statement);
    RUN(test_keywords_ignore_case_and_identifiers_do_not_match_prefixes);
    RUN(test_operators_and_numbers);
    RUN(test_strings_and_comments);
    RUN(test_invalid_input);
    return EXIT_SUCCESS;
}
