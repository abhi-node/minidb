#include "minidb/parser.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "minidb/lexer.h"
#include "minidb/row.h"

/* Bound on nested parentheses / NOT / unary minus, to keep recursion shallow. */
#define MAX_EXPRESSION_DEPTH 200

typedef struct {
    minidb_lexer_t lexer;
    minidb_token_t current;  /* the token being looked at; not yet consumed */
    minidb_arena_t *arena;
    minidb_error_t *error;
    int expression_depth;
} parser_t;

/* Every parse function returns a status and leaves `current` on the first
 * token it did not consume. */
#define TRY(expression)                        \
    do {                                       \
        minidb_status_t try_status_ = (expression); \
        if (try_status_ != MINIDB_OK) {        \
            return try_status_;                \
        }                                      \
    } while (0)

/* ---- Token helpers ------------------------------------------------------- */

static void advance(parser_t *parser) {
    parser->current = minidb_lexer_next(&parser->lexer);
}

static bool check(const parser_t *parser, minidb_token_type_t type) {
    return parser->current.type == type;
}

/* Consumes the current token if it has the given type. */
static bool match(parser_t *parser, minidb_token_type_t type) {
    if (!check(parser, type)) {
        return false;
    }
    advance(parser);
    return true;
}

static minidb_status_t syntax_error(parser_t *parser, const char *expected) {
    if (check(parser, MINIDB_TOKEN_END)) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                "expected %s but the statement ended", expected);
    }
    return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX, "expected %s near '%.*s'",
                            expected, (int)parser->current.length, parser->current.text);
}

static minidb_status_t expect(parser_t *parser, minidb_token_type_t type, const char *expected) {
    return match(parser, type) ? MINIDB_OK : syntax_error(parser, expected);
}

/* Consumes an identifier and returns a lower-cased copy of it. */
static minidb_status_t parse_name(parser_t *parser, const char *what, const char **out_name) {
    if (!check(parser, MINIDB_TOKEN_IDENTIFIER)) {
        return syntax_error(parser, what);
    }
    if (parser->current.length > MINIDB_MAX_NAME_LENGTH) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                "name '%.*s' is longer than %d characters",
                                (int)parser->current.length, parser->current.text,
                                MINIDB_MAX_NAME_LENGTH);
    }
    char *name =
        minidb_arena_copy_string(parser->arena, parser->current.text, parser->current.length);
    if (name == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    for (char *character = name; *character != '\0'; character++) {
        if (*character >= 'A' && *character <= 'Z') {
            *character = (char)(*character + ('a' - 'A'));
        }
    }
    advance(parser);
    *out_name = name;
    return MINIDB_OK;
}

/*
 * Appends one item to an arena-backed array, doubling it when full.
 * Capacity is implied by the count: 4, then every power of two.
 */
static minidb_status_t list_append(parser_t *parser, void *items_pointer, uint32_t *count,
                                   size_t item_size, const void *item) {
    void **items = items_pointer;
    bool is_full = *count == 0 || (*count >= 4 && (*count & (*count - 1)) == 0);
    if (is_full) {
        uint32_t capacity = *count == 0 ? 4 : *count * 2;
        void *grown = minidb_arena_alloc(parser->arena, capacity * item_size);
        if (grown == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        if (*count > 0) {
            memcpy(grown, *items, *count * item_size);
        }
        *items = grown;
    }
    memcpy((char *)*items + *count * item_size, item, item_size);
    (*count)++;
    return MINIDB_OK;
}

/* ---- Expressions --------------------------------------------------------- */

static minidb_status_t parse_expression(parser_t *parser, minidb_expr_t **out_expr);

static minidb_expr_t *new_expr(parser_t *parser, minidb_expr_kind_t kind) {
    minidb_expr_t *expr = minidb_arena_alloc_zeroed(parser->arena, sizeof(*expr));
    if (expr != NULL) {
        expr->kind = kind;
    }
    return expr;
}

static minidb_status_t new_binary(parser_t *parser, minidb_binary_op_t op, minidb_expr_t *left,
                                  minidb_expr_t *right, minidb_expr_t **out_expr) {
    minidb_expr_t *expr = new_expr(parser, MINIDB_EXPR_BINARY);
    if (expr == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    expr->as.binary.op = op;
    expr->as.binary.left = left;
    expr->as.binary.right = right;
    *out_expr = expr;
    return MINIDB_OK;
}

static minidb_status_t new_unary(parser_t *parser, minidb_unary_op_t op, minidb_expr_t *operand,
                                 minidb_expr_t **out_expr) {
    minidb_expr_t *expr = new_expr(parser, MINIDB_EXPR_UNARY);
    if (expr == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    expr->as.unary.op = op;
    expr->as.unary.operand = operand;
    *out_expr = expr;
    return MINIDB_OK;
}

static minidb_status_t enter_nested_expression(parser_t *parser) {
    if (++parser->expression_depth > MAX_EXPRESSION_DEPTH) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                "expression is nested too deeply");
    }
    return MINIDB_OK;
}

static minidb_status_t parse_number(parser_t *parser, minidb_value_t *out_value) {
    char digits[64];
    if (parser->current.length >= sizeof(digits)) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX, "number is too long");
    }
    memcpy(digits, parser->current.text, parser->current.length);
    digits[parser->current.length] = '\0';

    if (check(parser, MINIDB_TOKEN_DECIMAL)) {
        *out_value = minidb_value_decimal(strtod(digits, NULL));
    } else {
        errno = 0;
        long long integer = strtoll(digits, NULL, 10);
        if (errno == ERANGE) {
            return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                    "integer %s is out of range", digits);
        }
        *out_value = minidb_value_int(integer);
    }
    advance(parser);
    return MINIDB_OK;
}

/* Copies a quoted string literal, dropping the quotes and un-doubling ''. */
static minidb_status_t parse_string(parser_t *parser, minidb_value_t *out_value) {
    const char *quoted = parser->current.text + 1;
    uint32_t quoted_length = parser->current.length - 2;

    char *text = minidb_arena_alloc(parser->arena, quoted_length + 1);
    if (text == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    uint32_t text_length = 0;
    for (uint32_t index = 0; index < quoted_length; index++) {
        text[text_length++] = quoted[index];
        if (quoted[index] == '\'') {
            index++;  /* skip the second quote of the pair */
        }
    }
    if (text_length > MINIDB_MAX_TEXT_LENGTH) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                "string literal is longer than %d bytes",
                                MINIDB_MAX_TEXT_LENGTH);
    }
    *out_value = minidb_value_text(text, text_length);
    advance(parser);
    return MINIDB_OK;
}

static minidb_status_t parse_column_reference(parser_t *parser, minidb_expr_t *expr) {
    const char *first_name;
    TRY(parse_name(parser, "a column name", &first_name));
    if (match(parser, MINIDB_TOKEN_DOT)) {
        expr->as.column.table = first_name;
        return parse_name(parser, "a column name after '.'", &expr->as.column.name);
    }
    expr->as.column.table = NULL;
    expr->as.column.name = first_name;
    return MINIDB_OK;
}

static minidb_status_t parse_primary(parser_t *parser, minidb_expr_t **out_expr) {
    if (match(parser, MINIDB_TOKEN_LEFT_PAREN)) {
        TRY(enter_nested_expression(parser));
        TRY(parse_expression(parser, out_expr));
        parser->expression_depth--;
        return expect(parser, MINIDB_TOKEN_RIGHT_PAREN, "')'");
    }

    bool is_column = check(parser, MINIDB_TOKEN_IDENTIFIER);
    minidb_expr_t *expr = new_expr(parser, is_column ? MINIDB_EXPR_COLUMN : MINIDB_EXPR_LITERAL);
    if (expr == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    *out_expr = expr;

    switch (parser->current.type) {
        case MINIDB_TOKEN_IDENTIFIER:
            return parse_column_reference(parser, expr);
        case MINIDB_TOKEN_INTEGER:
        case MINIDB_TOKEN_DECIMAL:
            return parse_number(parser, &expr->as.literal);
        case MINIDB_TOKEN_STRING:
            return parse_string(parser, &expr->as.literal);
        case MINIDB_TOKEN_TRUE:
        case MINIDB_TOKEN_FALSE:
            expr->as.literal = minidb_value_bool(check(parser, MINIDB_TOKEN_TRUE));
            advance(parser);
            return MINIDB_OK;
        case MINIDB_TOKEN_INVALID:
            if (parser->current.text[0] == '\'') {
                return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                        "unterminated string literal");
            }
            return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                    "unexpected character '%.*s'", (int)parser->current.length,
                                    parser->current.text);
        default:
            return syntax_error(parser, "a value");
    }
}

static minidb_status_t parse_unary(parser_t *parser, minidb_expr_t **out_expr) {
    if (!match(parser, MINIDB_TOKEN_MINUS)) {
        return parse_primary(parser, out_expr);
    }
    minidb_expr_t *operand;
    TRY(enter_nested_expression(parser));
    TRY(parse_unary(parser, &operand));
    parser->expression_depth--;

    /* Fold the sign into numeric literals so -5 is a constant, not an operation. */
    if (operand->kind == MINIDB_EXPR_LITERAL && operand->as.literal.type == MINIDB_TYPE_INT) {
        operand->as.literal.as.integer = -operand->as.literal.as.integer;
        *out_expr = operand;
        return MINIDB_OK;
    }
    if (operand->kind == MINIDB_EXPR_LITERAL && operand->as.literal.type == MINIDB_TYPE_DECIMAL) {
        operand->as.literal.as.decimal = -operand->as.literal.as.decimal;
        *out_expr = operand;
        return MINIDB_OK;
    }
    return new_unary(parser, MINIDB_OP_NEGATE, operand, out_expr);
}

static minidb_status_t parse_product(parser_t *parser, minidb_expr_t **out_expr) {
    TRY(parse_unary(parser, out_expr));
    while (check(parser, MINIDB_TOKEN_STAR) || check(parser, MINIDB_TOKEN_SLASH)) {
        minidb_binary_op_t op =
            check(parser, MINIDB_TOKEN_STAR) ? MINIDB_OP_MULTIPLY : MINIDB_OP_DIVIDE;
        advance(parser);
        minidb_expr_t *right;
        TRY(parse_unary(parser, &right));
        TRY(new_binary(parser, op, *out_expr, right, out_expr));
    }
    return MINIDB_OK;
}

static minidb_status_t parse_sum(parser_t *parser, minidb_expr_t **out_expr) {
    TRY(parse_product(parser, out_expr));
    while (check(parser, MINIDB_TOKEN_PLUS) || check(parser, MINIDB_TOKEN_MINUS)) {
        minidb_binary_op_t op =
            check(parser, MINIDB_TOKEN_PLUS) ? MINIDB_OP_ADD : MINIDB_OP_SUBTRACT;
        advance(parser);
        minidb_expr_t *right;
        TRY(parse_product(parser, &right));
        TRY(new_binary(parser, op, *out_expr, right, out_expr));
    }
    return MINIDB_OK;
}

/* Maps a comparison token to its operator; false if it is not one. */
static bool comparison_operator(minidb_token_type_t type, minidb_binary_op_t *out_op) {
    switch (type) {
        case MINIDB_TOKEN_EQUAL:         *out_op = MINIDB_OP_EQUAL; return true;
        case MINIDB_TOKEN_NOT_EQUAL:     *out_op = MINIDB_OP_NOT_EQUAL; return true;
        case MINIDB_TOKEN_LESS:          *out_op = MINIDB_OP_LESS; return true;
        case MINIDB_TOKEN_LESS_EQUAL:    *out_op = MINIDB_OP_LESS_EQUAL; return true;
        case MINIDB_TOKEN_GREATER:       *out_op = MINIDB_OP_GREATER; return true;
        case MINIDB_TOKEN_GREATER_EQUAL: *out_op = MINIDB_OP_GREATER_EQUAL; return true;
        case MINIDB_TOKEN_LIKE:          *out_op = MINIDB_OP_LIKE; return true;
        default:                         return false;
    }
}

static minidb_status_t parse_comparison(parser_t *parser, minidb_expr_t **out_expr) {
    TRY(parse_sum(parser, out_expr));

    /* "a NOT LIKE b" is the one place NOT follows an operand. */
    bool is_negated = false;
    if (check(parser, MINIDB_TOKEN_NOT)) {
        advance(parser);
        if (!check(parser, MINIDB_TOKEN_LIKE)) {
            return syntax_error(parser, "LIKE after NOT");
        }
        is_negated = true;
    }

    minidb_binary_op_t op;
    if (!comparison_operator(parser->current.type, &op)) {
        return MINIDB_OK;
    }
    advance(parser);
    minidb_expr_t *right;
    TRY(parse_sum(parser, &right));
    TRY(new_binary(parser, op, *out_expr, right, out_expr));
    if (is_negated) {
        TRY(new_unary(parser, MINIDB_OP_NOT, *out_expr, out_expr));
    }
    return MINIDB_OK;
}

static minidb_status_t parse_not(parser_t *parser, minidb_expr_t **out_expr) {
    if (!match(parser, MINIDB_TOKEN_NOT)) {
        return parse_comparison(parser, out_expr);
    }
    minidb_expr_t *operand;
    TRY(enter_nested_expression(parser));
    TRY(parse_not(parser, &operand));
    parser->expression_depth--;
    return new_unary(parser, MINIDB_OP_NOT, operand, out_expr);
}

static minidb_status_t parse_and(parser_t *parser, minidb_expr_t **out_expr) {
    TRY(parse_not(parser, out_expr));
    while (match(parser, MINIDB_TOKEN_AND)) {
        minidb_expr_t *right;
        TRY(parse_not(parser, &right));
        TRY(new_binary(parser, MINIDB_OP_AND, *out_expr, right, out_expr));
    }
    return MINIDB_OK;
}

static minidb_status_t parse_expression(parser_t *parser, minidb_expr_t **out_expr) {
    TRY(parse_and(parser, out_expr));
    while (match(parser, MINIDB_TOKEN_OR)) {
        minidb_expr_t *right;
        TRY(parse_and(parser, &right));
        TRY(new_binary(parser, MINIDB_OP_OR, *out_expr, right, out_expr));
    }
    return MINIDB_OK;
}

/* ---- SELECT -------------------------------------------------------------- */

/* Parses "[AS] name" if present; leaves *out_alias untouched otherwise. */
static minidb_status_t parse_optional_alias(parser_t *parser, const char **out_alias) {
    if (match(parser, MINIDB_TOKEN_AS)) {
        return parse_name(parser, "a name after AS", out_alias);
    }
    if (check(parser, MINIDB_TOKEN_IDENTIFIER)) {
        return parse_name(parser, "an alias", out_alias);
    }
    return MINIDB_OK;
}

static minidb_status_t parse_table_ref(parser_t *parser, minidb_table_ref_t *out_table) {
    TRY(parse_name(parser, "a table name", &out_table->name));
    out_table->alias = out_table->name;
    return parse_optional_alias(parser, &out_table->alias);
}

static minidb_status_t parse_select_items(parser_t *parser, minidb_select_statement_t *select) {
    if (match(parser, MINIDB_TOKEN_STAR)) {
        select->selects_all_columns = true;
        return MINIDB_OK;
    }
    do {
        minidb_select_item_t item = {NULL, NULL};
        TRY(parse_expression(parser, &item.expr));
        TRY(parse_optional_alias(parser, &item.alias));
        TRY(list_append(parser, &select->items, &select->item_count, sizeof(item), &item));
    } while (match(parser, MINIDB_TOKEN_COMMA));
    return MINIDB_OK;
}

static minidb_status_t parse_joins(parser_t *parser, minidb_select_statement_t *select) {
    for (;;) {
        bool has_inner = match(parser, MINIDB_TOKEN_INNER);
        if (!match(parser, MINIDB_TOKEN_JOIN)) {
            return has_inner ? syntax_error(parser, "JOIN after INNER") : MINIDB_OK;
        }
        minidb_join_t join;
        TRY(parse_table_ref(parser, &join.table));
        TRY(expect(parser, MINIDB_TOKEN_ON, "ON"));
        TRY(parse_expression(parser, &join.condition));
        TRY(list_append(parser, &select->joins, &select->join_count, sizeof(join), &join));
    }
}

static minidb_status_t parse_order_by(parser_t *parser, minidb_select_statement_t *select) {
    TRY(expect(parser, MINIDB_TOKEN_BY, "BY after ORDER"));
    do {
        minidb_order_term_t term = {NULL, false};
        TRY(parse_expression(parser, &term.expr));
        if (match(parser, MINIDB_TOKEN_DESC)) {
            term.is_descending = true;
        } else {
            match(parser, MINIDB_TOKEN_ASC);
        }
        TRY(list_append(parser, &select->order_terms, &select->order_term_count, sizeof(term),
                        &term));
    } while (match(parser, MINIDB_TOKEN_COMMA));
    return MINIDB_OK;
}

static minidb_status_t parse_limit(parser_t *parser, minidb_select_statement_t *select) {
    if (!check(parser, MINIDB_TOKEN_INTEGER)) {
        return syntax_error(parser, "a row count after LIMIT");
    }
    minidb_value_t count;
    TRY(parse_number(parser, &count));
    select->has_limit = true;
    select->limit = count.as.integer;
    return MINIDB_OK;
}

static minidb_status_t parse_select(parser_t *parser, minidb_select_statement_t *select) {
    TRY(parse_select_items(parser, select));
    TRY(expect(parser, MINIDB_TOKEN_FROM, "FROM"));
    TRY(parse_table_ref(parser, &select->from));
    TRY(parse_joins(parser, select));
    if (match(parser, MINIDB_TOKEN_WHERE)) {
        TRY(parse_expression(parser, &select->where));
    }
    if (match(parser, MINIDB_TOKEN_ORDER)) {
        TRY(parse_order_by(parser, select));
    }
    if (match(parser, MINIDB_TOKEN_LIMIT)) {
        TRY(parse_limit(parser, select));
    }
    return MINIDB_OK;
}

/* ---- INSERT / UPDATE / DELETE -------------------------------------------- */

static minidb_status_t parse_insert_row(parser_t *parser, minidb_insert_statement_t *insert,
                                        uint32_t *value_count) {
    uint32_t values_in_row = 0;
    TRY(expect(parser, MINIDB_TOKEN_LEFT_PAREN, "'(' before the values"));
    do {
        minidb_expr_t *value;
        TRY(parse_expression(parser, &value));
        TRY(list_append(parser, &insert->values, value_count, sizeof(value), &value));
        values_in_row++;
    } while (match(parser, MINIDB_TOKEN_COMMA));
    TRY(expect(parser, MINIDB_TOKEN_RIGHT_PAREN, "')' after the values"));

    if (insert->row_count == 0) {
        insert->values_per_row = values_in_row;
    } else if (values_in_row != insert->values_per_row) {
        return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                "row %u has %u values but the first row has %u",
                                insert->row_count + 1, values_in_row, insert->values_per_row);
    }
    insert->row_count++;
    return MINIDB_OK;
}

static minidb_status_t parse_insert(parser_t *parser, minidb_insert_statement_t *insert) {
    TRY(expect(parser, MINIDB_TOKEN_INTO, "INTO"));
    TRY(parse_name(parser, "a table name", &insert->table_name));

    if (match(parser, MINIDB_TOKEN_LEFT_PAREN)) {
        do {
            const char *column_name;
            TRY(parse_name(parser, "a column name", &column_name));
            TRY(list_append(parser, &insert->column_names, &insert->column_name_count,
                            sizeof(column_name), &column_name));
        } while (match(parser, MINIDB_TOKEN_COMMA));
        TRY(expect(parser, MINIDB_TOKEN_RIGHT_PAREN, "')' after the column list"));
    }

    TRY(expect(parser, MINIDB_TOKEN_VALUES, "VALUES"));
    uint32_t value_count = 0;
    do {
        TRY(parse_insert_row(parser, insert, &value_count));
    } while (match(parser, MINIDB_TOKEN_COMMA));
    return MINIDB_OK;
}

static minidb_status_t parse_update(parser_t *parser, minidb_update_statement_t *update) {
    TRY(parse_name(parser, "a table name", &update->table_name));
    TRY(expect(parser, MINIDB_TOKEN_SET, "SET"));
    do {
        minidb_assignment_t assignment;
        TRY(parse_name(parser, "a column name", &assignment.column_name));
        TRY(expect(parser, MINIDB_TOKEN_EQUAL, "'='"));
        TRY(parse_expression(parser, &assignment.value));
        TRY(list_append(parser, &update->assignments, &update->assignment_count,
                        sizeof(assignment), &assignment));
    } while (match(parser, MINIDB_TOKEN_COMMA));

    if (match(parser, MINIDB_TOKEN_WHERE)) {
        TRY(parse_expression(parser, &update->where));
    }
    return MINIDB_OK;
}

static minidb_status_t parse_delete(parser_t *parser, minidb_delete_statement_t *delete_rows) {
    TRY(expect(parser, MINIDB_TOKEN_FROM, "FROM"));
    TRY(parse_name(parser, "a table name", &delete_rows->table_name));
    if (match(parser, MINIDB_TOKEN_WHERE)) {
        TRY(parse_expression(parser, &delete_rows->where));
    }
    return MINIDB_OK;
}

/* ---- CREATE / DROP TABLE ------------------------------------------------- */

static minidb_status_t parse_column_type(parser_t *parser, minidb_type_t *out_type) {
    static const struct {
        const char *name;
        minidb_type_t type;
    } type_names[] = {
        {"int", MINIDB_TYPE_INT},         {"integer", MINIDB_TYPE_INT},
        {"decimal", MINIDB_TYPE_DECIMAL}, {"real", MINIDB_TYPE_DECIMAL},
        {"double", MINIDB_TYPE_DECIMAL},  {"float", MINIDB_TYPE_DECIMAL},
        {"text", MINIDB_TYPE_TEXT},       {"varchar", MINIDB_TYPE_TEXT},
        {"bool", MINIDB_TYPE_BOOL},       {"boolean", MINIDB_TYPE_BOOL},
    };

    const char *type_name;
    TRY(parse_name(parser, "a column type", &type_name));
    for (size_t index = 0; index < sizeof(type_names) / sizeof(type_names[0]); index++) {
        if (strcmp(type_name, type_names[index].name) != 0) {
            continue;
        }
        *out_type = type_names[index].type;
        /* VARCHAR(n): the length is accepted and ignored. */
        if (match(parser, MINIDB_TOKEN_LEFT_PAREN)) {
            TRY(expect(parser, MINIDB_TOKEN_INTEGER, "a length"));
            TRY(expect(parser, MINIDB_TOKEN_RIGHT_PAREN, "')'"));
        }
        return MINIDB_OK;
    }
    return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX, "unknown column type '%s'",
                            type_name);
}

static minidb_status_t parse_column_definition(parser_t *parser, minidb_schema_t *schema) {
    const char *column_name;
    minidb_type_t type = MINIDB_TYPE_INT;
    TRY(parse_name(parser, "a column name", &column_name));
    TRY(parse_column_type(parser, &type));

    bool is_primary_key = false;
    if (match(parser, MINIDB_TOKEN_PRIMARY)) {
        TRY(expect(parser, MINIDB_TOKEN_KEY, "KEY after PRIMARY"));
        is_primary_key = true;
    }

    switch (minidb_schema_add_column(schema, column_name, type, is_primary_key)) {
        case MINIDB_OK:
            return MINIDB_OK;
        case MINIDB_ERR_INVALID_ARGUMENT:
            return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                    "column '%s' is defined twice", column_name);
        case MINIDB_ERR_TYPE_MISMATCH:
            return minidb_error_set(parser->error, MINIDB_ERR_UNSUPPORTED,
                                    "primary key '%s' must be an INT", column_name);
        default:
            return minidb_error_set(parser->error, MINIDB_ERR_UNSUPPORTED,
                                    "a table can have one primary key and at most %d columns",
                                    MINIDB_MAX_COLUMNS);
    }
}

static minidb_status_t parse_create_table(parser_t *parser, minidb_schema_t **out_schema) {
    const char *table_name;
    TRY(expect(parser, MINIDB_TOKEN_TABLE, "TABLE"));
    TRY(parse_name(parser, "a table name", &table_name));

    minidb_schema_t *schema = minidb_arena_alloc(parser->arena, sizeof(*schema));
    if (schema == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    TRY(minidb_schema_init(schema, table_name));

    TRY(expect(parser, MINIDB_TOKEN_LEFT_PAREN, "'(' before the columns"));
    do {
        TRY(parse_column_definition(parser, schema));
    } while (match(parser, MINIDB_TOKEN_COMMA));
    TRY(expect(parser, MINIDB_TOKEN_RIGHT_PAREN, "')' after the columns"));

    *out_schema = schema;
    return MINIDB_OK;
}

/* ---- Statements ---------------------------------------------------------- */

static minidb_status_t parse_statement_body(parser_t *parser, minidb_statement_t *statement) {
    minidb_token_type_t keyword = parser->current.type;
    advance(parser);

    switch (keyword) {
        case MINIDB_TOKEN_SELECT:
            statement->kind = MINIDB_STATEMENT_SELECT;
            return parse_select(parser, &statement->as.select);
        case MINIDB_TOKEN_INSERT:
            statement->kind = MINIDB_STATEMENT_INSERT;
            return parse_insert(parser, &statement->as.insert);
        case MINIDB_TOKEN_UPDATE:
            statement->kind = MINIDB_STATEMENT_UPDATE;
            return parse_update(parser, &statement->as.update);
        case MINIDB_TOKEN_DELETE:
            statement->kind = MINIDB_STATEMENT_DELETE;
            return parse_delete(parser, &statement->as.delete_rows);
        case MINIDB_TOKEN_CREATE:
            statement->kind = MINIDB_STATEMENT_CREATE_TABLE;
            return parse_create_table(parser, &statement->as.create_table);
        case MINIDB_TOKEN_DROP:
            statement->kind = MINIDB_STATEMENT_DROP_TABLE;
            TRY(expect(parser, MINIDB_TOKEN_TABLE, "TABLE"));
            return parse_name(parser, "a table name", &statement->as.drop_table);
        default:
            return minidb_error_set(parser->error, MINIDB_ERR_SYNTAX,
                                    "expected SELECT, INSERT, UPDATE, DELETE, CREATE or DROP");
    }
}

minidb_status_t minidb_parse_statement(const char *sql, size_t length, minidb_arena_t *arena,
                                       minidb_statement_t **out_statement, size_t *out_consumed,
                                       minidb_error_t *error) {
    parser_t parser = {.arena = arena, .error = error};
    minidb_lexer_init(&parser.lexer, sql, length);
    advance(&parser);

    *out_statement = NULL;
    *out_consumed = length;

    while (match(&parser, MINIDB_TOKEN_SEMICOLON)) {
        /* empty statement */
    }
    if (check(&parser, MINIDB_TOKEN_END)) {
        return MINIDB_OK;
    }

    minidb_statement_t *statement = minidb_arena_alloc_zeroed(arena, sizeof(*statement));
    if (statement == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    TRY(parse_statement_body(&parser, statement));

    if (check(&parser, MINIDB_TOKEN_SEMICOLON)) {
        *out_consumed = (size_t)(parser.current.text - sql) + 1;
    } else if (!check(&parser, MINIDB_TOKEN_END)) {
        return syntax_error(&parser, "';' or the end of the statement");
    }
    *out_statement = statement;
    return MINIDB_OK;
}
