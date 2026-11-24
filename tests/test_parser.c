#include "test.h"

#include <inttypes.h>

#include "minidb/parser.h"

static minidb_arena_t arena;
static minidb_error_t error;

static minidb_statement_t *parse(const char *sql) {
    minidb_statement_t *statement;
    size_t consumed;
    minidb_arena_reset(&arena);
    minidb_status_t status =
        minidb_parse_statement(sql, strlen(sql), &arena, &statement, &consumed, &error);
    if (status != MINIDB_OK) {
        fprintf(stderr, "parse of \"%s\" failed: %s\n", sql, error.message);
        exit(EXIT_FAILURE);
    }
    return statement;
}

static void check_syntax_error(const char *sql, const char *expected_fragment) {
    minidb_statement_t *statement;
    size_t consumed;
    minidb_arena_reset(&arena);
    minidb_status_t status =
        minidb_parse_statement(sql, strlen(sql), &arena, &statement, &consumed, &error);
    if (status == MINIDB_OK || strstr(error.message, expected_fragment) == NULL) {
        fprintf(stderr, "\"%s\": expected an error containing \"%s\", got \"%s\"\n", sql,
                expected_fragment, status == MINIDB_OK ? "(parsed)" : error.message);
        exit(EXIT_FAILURE);
    }
}

/* Renders an expression fully parenthesized, to make precedence visible. */
static void render(const minidb_expr_t *expr, char *out, size_t capacity) {
    static const char *binary_symbols[] = {"+",  "-", "*",  "/",   "=",  "!=",  "<",
                                           "<=", ">", ">=", "AND", "OR", "LIKE"};
    size_t used = strlen(out);
    switch (expr->kind) {
        case MINIDB_EXPR_LITERAL:
            switch (expr->as.literal.type) {
                case MINIDB_TYPE_INT:
                    snprintf(out + used, capacity - used, "%" PRId64, expr->as.literal.as.integer);
                    break;
                case MINIDB_TYPE_DECIMAL:
                    snprintf(out + used, capacity - used, "%g", expr->as.literal.as.decimal);
                    break;
                case MINIDB_TYPE_TEXT:
                    snprintf(out + used, capacity - used, "'%.*s'",
                             (int)expr->as.literal.as.text.length,
                             expr->as.literal.as.text.bytes);
                    break;
                case MINIDB_TYPE_BOOL:
                    snprintf(out + used, capacity - used, "%s",
                             expr->as.literal.as.boolean ? "true" : "false");
                    break;
            }
            break;
        case MINIDB_EXPR_COLUMN:
            if (expr->as.column.table != NULL) {
                snprintf(out + used, capacity - used, "%s.%s", expr->as.column.table,
                         expr->as.column.name);
            } else {
                snprintf(out + used, capacity - used, "%s", expr->as.column.name);
            }
            break;
        case MINIDB_EXPR_UNARY:
            snprintf(out + used, capacity - used, "(%s ",
                     expr->as.unary.op == MINIDB_OP_NOT ? "NOT" : "-");
            render(expr->as.unary.operand, out, capacity);
            snprintf(out + strlen(out), capacity - strlen(out), ")");
            break;
        case MINIDB_EXPR_BINARY:
            snprintf(out + used, capacity - used, "(");
            render(expr->as.binary.left, out, capacity);
            snprintf(out + strlen(out), capacity - strlen(out), " %s ",
                     binary_symbols[expr->as.binary.op]);
            render(expr->as.binary.right, out, capacity);
            snprintf(out + strlen(out), capacity - strlen(out), ")");
            break;
    }
}

static void check_where(const char *condition, const char *expected) {
    char sql[512];
    char rendered[512] = "";
    snprintf(sql, sizeof(sql), "SELECT * FROM t WHERE %s", condition);
    render(parse(sql)->as.select.where, rendered, sizeof(rendered));
    if (strcmp(rendered, expected) != 0) {
        fprintf(stderr, "\"%s\" parsed as %s, expected %s\n", condition, rendered, expected);
        exit(EXIT_FAILURE);
    }
}

static void test_expression_precedence(void) {
    check_where("a + b * 2 > 10", "((a + (b * 2)) > 10)");
    check_where("(a + b) * 2 = c", "(((a + b) * 2) = c)");
    check_where("a - b - c < 0", "(((a - b) - c) < 0)");
    check_where("a = 1 OR b = 2 AND c = 3", "((a = 1) OR ((b = 2) AND (c = 3)))");
    check_where("NOT a = 1 AND b", "((NOT (a = 1)) AND b)");
    check_where("t.x <> -5", "(t.x != -5)");
    check_where("-x < - 2.5", "((- x) < -2.5)");
    check_where("name LIKE 'A%' AND active = TRUE", "((name LIKE 'A%') AND (active = true))");
    check_where("name NOT LIKE '%@x.com'", "(NOT (name LIKE '%@x.com'))");
    check_where("note = 'it''s'", "(note = 'it's')");
}

static void test_select(void) {
    minidb_select_statement_t *select =
        &parse("SELECT u.name AS who, o.total * 2 double_total FROM Users u "
               "JOIN orders AS o ON o.user_id = u.id INNER JOIN items ON items.order_id = o.id "
               "WHERE o.total > 10 ORDER BY o.total DESC, u.name LIMIT 5;")
             ->as.select;

    CHECK(!select->selects_all_columns && select->item_count == 2);
    CHECK(strcmp(select->items[0].alias, "who") == 0);
    CHECK(strcmp(select->items[1].alias, "double_total") == 0);
    CHECK(strcmp(select->from.name, "users") == 0 && strcmp(select->from.alias, "u") == 0);
    CHECK(select->join_count == 2);
    CHECK(strcmp(select->joins[0].table.alias, "o") == 0);
    CHECK(strcmp(select->joins[1].table.alias, "items") == 0);
    CHECK(select->where != NULL);
    CHECK(select->order_term_count == 2);
    CHECK(select->order_terms[0].is_descending && !select->order_terms[1].is_descending);
    CHECK(select->has_limit && select->limit == 5);

    select = &parse("select * from t")->as.select;
    CHECK(select->selects_all_columns && select->where == NULL && !select->has_limit);
}

static void test_insert(void) {
    minidb_insert_statement_t *insert =
        &parse("INSERT INTO users (id, name) VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol')")
             ->as.insert;
    CHECK(strcmp(insert->table_name, "users") == 0);
    CHECK(insert->column_name_count == 2 && strcmp(insert->column_names[1], "name") == 0);
    CHECK(insert->row_count == 3 && insert->values_per_row == 2);
    CHECK(insert->values[5]->as.literal.as.text.length == 5);

    insert = &parse("INSERT INTO t VALUES (1, 2.5, TRUE, -4)")->as.insert;
    CHECK(insert->column_names == NULL && insert->row_count == 1 && insert->values_per_row == 4);
    CHECK(insert->values[3]->as.literal.as.integer == -4);
}

static void test_update_and_delete(void) {
    minidb_statement_t *statement = parse("UPDATE users SET age = age + 1, name = 'x' WHERE id = 7");
    CHECK(statement->kind == MINIDB_STATEMENT_UPDATE);
    CHECK(statement->as.update.assignment_count == 2);
    CHECK(strcmp(statement->as.update.assignments[0].column_name, "age") == 0);
    CHECK(statement->as.update.where != NULL);

    statement = parse("DELETE FROM users");
    CHECK(statement->kind == MINIDB_STATEMENT_DELETE && statement->as.delete_rows.where == NULL);
}

static void test_create_and_drop(void) {
    minidb_statement_t *statement = parse(
        "CREATE TABLE Users (id INTEGER PRIMARY KEY, name VARCHAR(40), score REAL, ok BOOLEAN)");
    CHECK(statement->kind == MINIDB_STATEMENT_CREATE_TABLE);
    const minidb_schema_t *schema = statement->as.create_table;
    CHECK(strcmp(schema->table_name, "users") == 0);
    CHECK(schema->column_count == 4 && schema->primary_key_index == 0);
    CHECK(schema->columns[1].type == MINIDB_TYPE_TEXT);
    CHECK(schema->columns[2].type == MINIDB_TYPE_DECIMAL);
    CHECK(schema->columns[3].type == MINIDB_TYPE_BOOL);

    statement = parse("DROP TABLE users;");
    CHECK(statement->kind == MINIDB_STATEMENT_DROP_TABLE);
    CHECK(strcmp(statement->as.drop_table, "users") == 0);
}

static void test_statements_are_consumed_one_at_a_time(void) {
    const char *sql = " ;; SELECT * FROM a; -- note\n DELETE FROM b ;  ";
    size_t length = strlen(sql);
    size_t offset = 0;
    int statement_count = 0;

    minidb_arena_reset(&arena);
    for (;;) {
        minidb_statement_t *statement;
        size_t consumed;
        CHECK_OK(minidb_parse_statement(sql + offset, length - offset, &arena, &statement,
                                        &consumed, &error));
        if (statement == NULL) {
            break;
        }
        statement_count++;
        offset += consumed;
    }
    CHECK(statement_count == 2);
}

static void test_syntax_errors(void) {
    check_syntax_error("SELEC * FROM t", "expected SELECT");
    check_syntax_error("SELECT * t", "expected FROM near 't'");
    check_syntax_error("SELECT * FROM t WHERE", "the statement ended");
    check_syntax_error("SELECT * FROM t WHERE (a = 1", "expected ')'");
    check_syntax_error("SELECT * FROM t extra tokens", "expected ';'");
    check_syntax_error("SELECT * FROM t WHERE name = 'open", "unterminated string");
    check_syntax_error("SELECT a @ b FROM t", "expected FROM near '@'");
    check_syntax_error("INSERT INTO t VALUES (1, 2), (3)", "row 2 has 1 values");
    check_syntax_error("CREATE TABLE t (a INT, a TEXT)", "defined twice");
    check_syntax_error("CREATE TABLE t (a BLOB)", "unknown column type");
    check_syntax_error("CREATE TABLE t (a TEXT PRIMARY KEY)", "must be an INT");
    check_syntax_error("SELECT * FROM t LIMIT many", "row count");
    check_syntax_error("SELECT * FROM t WHERE x = 99999999999999999999", "out of range");

    /* Deep nesting is rejected instead of overflowing the stack. */
    char nested[2048] = "SELECT * FROM t WHERE ";
    for (int depth = 0; depth < 500; depth++) {
        strcat(nested, "(");
    }
    check_syntax_error(nested, "nested too deeply");
}

int main(void) {
    minidb_arena_init(&arena);
    RUN(test_expression_precedence);
    RUN(test_select);
    RUN(test_insert);
    RUN(test_update_and_delete);
    RUN(test_create_and_drop);
    RUN(test_statements_are_consumed_one_at_a_time);
    RUN(test_syntax_errors);
    minidb_arena_destroy(&arena);
    return EXIT_SUCCESS;
}
