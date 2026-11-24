#include "test.h"

#include "minidb/arena.h"
#include "minidb/row.h"
#include "minidb/schema.h"

static minidb_schema_t accounts_schema(void) {
    minidb_schema_t schema;
    CHECK_OK(minidb_schema_init(&schema, "accounts"));
    CHECK_OK(minidb_schema_add_column(&schema, "id", MINIDB_TYPE_INT, true));
    CHECK_OK(minidb_schema_add_column(&schema, "name", MINIDB_TYPE_TEXT, false));
    CHECK_OK(minidb_schema_add_column(&schema, "balance", MINIDB_TYPE_DECIMAL, false));
    CHECK_OK(minidb_schema_add_column(&schema, "active", MINIDB_TYPE_BOOL, false));
    return schema;
}

static void test_schema_rejects_bad_columns(void) {
    minidb_schema_t schema = accounts_schema();
    CHECK(schema.primary_key_index == 0);
    CHECK(minidb_schema_find_column(&schema, "balance") == 2);
    CHECK(minidb_schema_find_column(&schema, "missing") == -1);

    CHECK_STATUS(MINIDB_ERR_INVALID_ARGUMENT,
                 minidb_schema_add_column(&schema, "name", MINIDB_TYPE_INT, false));
    CHECK_STATUS(MINIDB_ERR_UNSUPPORTED,
                 minidb_schema_add_column(&schema, "other_id", MINIDB_TYPE_INT, true));

    minidb_schema_t text_keyed;
    CHECK_OK(minidb_schema_init(&text_keyed, "t"));
    CHECK_STATUS(MINIDB_ERR_TYPE_MISMATCH,
                 minidb_schema_add_column(&text_keyed, "k", MINIDB_TYPE_TEXT, true));
}

static void test_schema_round_trip(void) {
    minidb_schema_t schema = accounts_schema();
    uint8_t bytes[512];
    size_t size = minidb_schema_encoded_size(&schema);
    CHECK(size <= sizeof(bytes));
    minidb_schema_encode(&schema, bytes);

    minidb_schema_t decoded;
    CHECK_OK(minidb_schema_decode(bytes, size, &decoded));
    CHECK(memcmp(&schema, &decoded, sizeof(schema)) == 0);

    CHECK_STATUS(MINIDB_ERR_CORRUPT, minidb_schema_decode(bytes, size - 1, &decoded));
}

static void test_row_round_trip(void) {
    minidb_schema_t schema = accounts_schema();
    minidb_value_t values[4] = {
        minidb_value_int(-42),
        minidb_value_text("Abhijith", 8),
        minidb_value_decimal(1234.5),
        minidb_value_bool(true),
    };

    uint8_t bytes[64];
    size_t size = minidb_row_encoded_size(&schema, values);
    CHECK(size == 8 + (2 + 8) + 8 + 1);
    minidb_row_encode(&schema, values, bytes);

    minidb_value_t decoded[4];
    CHECK_OK(minidb_row_decode(&schema, bytes, size, decoded));
    for (int index = 0; index < 4; index++) {
        CHECK(decoded[index].type == values[index].type);
        CHECK(minidb_value_compare(&decoded[index], &values[index]) == 0);
    }
    /* TEXT is decoded in place, not copied. */
    CHECK((const uint8_t *)decoded[1].as.text.bytes == bytes + 8 + 2);

    CHECK_STATUS(MINIDB_ERR_CORRUPT, minidb_row_decode(&schema, bytes, size - 1, decoded));
    CHECK_STATUS(MINIDB_ERR_CORRUPT, minidb_row_decode(&schema, bytes, size + 1, decoded));
}

static void test_value_compare(void) {
    minidb_value_t two = minidb_value_int(2);
    minidb_value_t two_and_a_half = minidb_value_decimal(2.5);
    minidb_value_t apple = minidb_value_text("apple", 5);
    minidb_value_t apples = minidb_value_text("apples", 6);

    CHECK(minidb_value_compare(&two, &two_and_a_half) < 0);
    CHECK(minidb_value_compare(&two_and_a_half, &two) > 0);
    CHECK(minidb_value_compare(&apple, &apples) < 0);
    CHECK(minidb_types_comparable(MINIDB_TYPE_INT, MINIDB_TYPE_DECIMAL));
    CHECK(!minidb_types_comparable(MINIDB_TYPE_INT, MINIDB_TYPE_TEXT));
}

static void test_arena_reuses_first_block(void) {
    minidb_arena_t arena;
    minidb_arena_init(&arena);

    void *first = minidb_arena_alloc(&arena, 100);
    CHECK(first != NULL);
    CHECK(minidb_arena_alloc(&arena, 100000) != NULL);  /* forces a second block */
    char *copy = minidb_arena_copy_string(&arena, "hello", 5);
    CHECK(strcmp(copy, "hello") == 0);

    minidb_arena_reset(&arena);
    CHECK(minidb_arena_alloc(&arena, 100) == first);

    minidb_arena_destroy(&arena);
}

int main(void) {
    RUN(test_schema_rejects_bad_columns);
    RUN(test_schema_round_trip);
    RUN(test_row_round_trip);
    RUN(test_value_compare);
    RUN(test_arena_reuses_first_block);
    return EXIT_SUCCESS;
}
