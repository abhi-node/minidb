#include "test.h"

#include "minidb/expression.h"
#include "minidb/parser.h"

static minidb_arena_t arena;
static minidb_error_t error;

/* Rows look like: users(id INT, name TEXT, age INT, score DECIMAL, active BOOL). */
static minidb_field_t user_fields[] = {
    {"users", "id", MINIDB_TYPE_INT},        {"users", "name", MINIDB_TYPE_TEXT},
    {"users", "age", MINIDB_TYPE_INT},       {"users", "score", MINIDB_TYPE_DECIMAL},
    {"users", "active", MINIDB_TYPE_BOOL},
};
static const minidb_row_shape_t user_shape = {user_fields, 5};

/* Parses `text` as the WHERE of a SELECT, which accepts any expression. */
static minidb_expr_t *parse_expression(const char *text) {
    char sql[512];
    minidb_statement_t *statement;
    size_t consumed;
    snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE %s", text);
    CHECK_OK(minidb_parse_statement(sql, strlen(sql), &arena, &statement, &consumed, &error));
    return statement->as.select.where;
}

static minidb_value_t evaluate(const char *text, const minidb_value_t *row) {
    minidb_expr_t *expr = parse_expression(text);
    minidb_value_t value;
    if (minidb_expr_bind(expr, &user_shape, &error) != MINIDB_OK) {
        fprintf(stderr, "bind of \"%s\" failed: %s\n", text, error.message);
        exit(EXIT_FAILURE);
    }
    CHECK_OK(minidb_expr_evaluate(expr, row, &value));
    return value;
}

static void check_true(const char *text, const minidb_value_t *row, bool expected) {
    minidb_value_t value = evaluate(text, row);
    if (value.type != MINIDB_TYPE_BOOL || value.as.boolean != expected) {
        fprintf(stderr, "\"%s\" should be %s\n", text, expected ? "true" : "false");
        exit(EXIT_FAILURE);
    }
}

static void check_bind_error(const char *text, minidb_status_t expected_status,
                             const char *expected_fragment) {
    minidb_expr_t *expr = parse_expression(text);
    minidb_status_t status = minidb_expr_bind(expr, &user_shape, &error);
    if (status != expected_status || strstr(error.message, expected_fragment) == NULL) {
        fprintf(stderr, "\"%s\": expected \"%s\", got status %s \"%s\"\n", text,
                expected_fragment, minidb_status_name(status), error.message);
        exit(EXIT_FAILURE);
    }
}

static const minidb_value_t *ada(void) {
    static minidb_value_t row[5];
    row[0] = minidb_value_int(7);
    row[1] = minidb_value_text("Ada Lovelace", 12);
    row[2] = minidb_value_int(36);
    row[3] = minidb_value_decimal(9.5);
    row[4] = minidb_value_bool(true);
    return row;
}

static void test_arithmetic_and_result_types(void) {
    minidb_value_t value = evaluate("age + 2 * 3", ada());
    CHECK(value.type == MINIDB_TYPE_INT && value.as.integer == 42);

    value = evaluate("age / 5", ada());
    CHECK(value.type == MINIDB_TYPE_INT && value.as.integer == 7);

    /* An INT mixed with a DECIMAL is promoted. */
    value = evaluate("age / 8.0", ada());
    CHECK(value.type == MINIDB_TYPE_DECIMAL && value.as.decimal == 4.5);

    value = evaluate("-(score - age)", ada());
    CHECK(value.type == MINIDB_TYPE_DECIMAL && value.as.decimal == 26.5);
}

static void test_comparisons_and_logic(void) {
    check_true("age > 18 AND name = 'Ada Lovelace'", ada(), true);
    check_true("age < 18 OR score >= 9.5", ada(), true);
    check_true("NOT active", ada(), false);
    check_true("active = TRUE AND id != 8", ada(), true);
    check_true("age = 36.0", ada(), true);
    check_true("name < 'Bob'", ada(), true);
    check_true("users.age <= 35", ada(), false);
}

static void test_like(void) {
    check_true("name LIKE 'Ada%'", ada(), true);
    check_true("name LIKE '%lace'", ada(), true);
    check_true("name LIKE '%Love%'", ada(), true);
    check_true("name LIKE 'A_a L%e'", ada(), true);
    check_true("name LIKE 'Ada'", ada(), false);
    check_true("name LIKE '%'", ada(), true);
    /* Two lower-case 'a's: backtracking must find both, and not a third. */
    check_true("name LIKE '%a%a%'", ada(), true);
    check_true("name LIKE '%a%a%a%'", ada(), false);
    check_true("name NOT LIKE '%Byron'", ada(), true);
}

static void test_short_circuit_skips_division_by_zero(void) {
    check_true("age = 36 OR 1 / (age - 36) > 0", ada(), true);
    check_true("age != 36 AND 1 / (age - 36) > 0", ada(), false);

    minidb_expr_t *expr = parse_expression("1 / (age - 36) > 0");
    minidb_value_t value;
    CHECK_OK(minidb_expr_bind(expr, &user_shape, &error));
    CHECK_STATUS(MINIDB_ERR_DIVISION_BY_ZERO, minidb_expr_evaluate(expr, ada(), &value));
}

static void test_type_errors_are_caught_at_bind_time(void) {
    check_bind_error("name > 5", MINIDB_ERR_TYPE_MISMATCH, "cannot apply > to TEXT and INT");
    check_bind_error("name + 1 = 2", MINIDB_ERR_TYPE_MISMATCH, "cannot apply + to TEXT and INT");
    check_bind_error("age AND active", MINIDB_ERR_TYPE_MISMATCH, "AND to INT and BOOL");
    check_bind_error("active < TRUE", MINIDB_ERR_TYPE_MISMATCH, "< to BOOL and BOOL");
    check_bind_error("NOT age", MINIDB_ERR_TYPE_MISMATCH, "NOT needs a BOOL");
    check_bind_error("-name = 'x'", MINIDB_ERR_TYPE_MISMATCH, "cannot negate TEXT");
    check_bind_error("age LIKE '3%'", MINIDB_ERR_TYPE_MISMATCH, "LIKE to INT and TEXT");

    minidb_expr_t *not_a_condition = parse_expression("age + 1");
    CHECK_STATUS(MINIDB_ERR_TYPE_MISMATCH,
                 minidb_expr_bind_predicate(not_a_condition, &user_shape, "WHERE", &error));
    CHECK(strstr(error.message, "WHERE must be a BOOL condition, not INT") != NULL);
}

static void test_column_resolution(void) {
    check_bind_error("height > 1", MINIDB_ERR_UNKNOWN_COLUMN, "no such column 'height'");
    check_bind_error("orders.id = 1", MINIDB_ERR_UNKNOWN_COLUMN, "no such column 'orders.id'");

    /* Two tables with an `id`: the bare name is ambiguous, qualified is not. */
    minidb_field_t joined_fields[] = {
        {"u", "id", MINIDB_TYPE_INT},
        {"o", "id", MINIDB_TYPE_INT},
        {"o", "user_id", MINIDB_TYPE_INT},
    };
    minidb_row_shape_t joined_shape = {joined_fields, 3};

    minidb_expr_t *ambiguous = parse_expression("id = 1");
    CHECK_STATUS(MINIDB_ERR_UNKNOWN_COLUMN, minidb_expr_bind(ambiguous, &joined_shape, &error));
    CHECK(strstr(error.message, "ambiguous") != NULL);

    minidb_expr_t *qualified = parse_expression("o.user_id = u.id");
    CHECK_OK(minidb_expr_bind(qualified, &joined_shape, &error));
    CHECK(qualified->as.binary.left->as.column.index == 2);
    CHECK(qualified->as.binary.right->as.column.index == 0);

    /* With no shape at all (INSERT values), any column is an error. */
    minidb_expr_t *constant_only = parse_expression("age = 1");
    CHECK_STATUS(MINIDB_ERR_UNKNOWN_COLUMN, minidb_expr_bind(constant_only, NULL, &error));
}

int main(void) {
    minidb_arena_init(&arena);
    RUN(test_arithmetic_and_result_types);
    RUN(test_comparisons_and_logic);
    RUN(test_like);
    RUN(test_short_circuit_skips_division_by_zero);
    RUN(test_type_errors_are_caught_at_bind_time);
    RUN(test_column_resolution);
    minidb_arena_destroy(&arena);
    return EXIT_SUCCESS;
}
