#include "test.h"

#include "minidb/btree.h"

/* Deterministic pseudo-random numbers so failures reproduce. */
static uint64_t random_state = 0x9E3779B97F4A7C15ull;
static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

typedef struct {
    char path[256];
    minidb_pager_t *pager;
    minidb_page_id_t root;
} tree_fixture_t;

static tree_fixture_t fixture_open(const char *name) {
    tree_fixture_t fixture;
    test_scratch_path(fixture.path, sizeof(fixture.path), name);
    CHECK_OK(minidb_pager_open(fixture.path, 64, &fixture.pager));
    CHECK_OK(minidb_btree_create(fixture.pager, &fixture.root));
    return fixture;
}

static void fixture_close(tree_fixture_t *fixture) {
    minidb_pager_close(fixture->pager);
    test_remove_database(fixture->path);
}

/* The payload for a key is `length` bytes derived from the key. */
static uint32_t make_payload(int64_t key, uint32_t length, uint8_t *out_payload) {
    for (uint32_t index = 0; index < length; index++) {
        out_payload[index] = (uint8_t)(key * 31 + index);
    }
    return length;
}

static void check_payload(const minidb_cursor_t *cursor, int64_t key, uint32_t expected_length) {
    uint8_t expected[MINIDB_BTREE_MAX_PAYLOAD];
    uint32_t length;
    const uint8_t *payload = minidb_cursor_payload(cursor, &length);
    make_payload(key, expected_length, expected);
    CHECK(length == expected_length);
    CHECK(memcmp(payload, expected, length) == 0);
}

/* Counts entries and checks that a full scan returns strictly ascending keys. */
static uint64_t scan_and_count(tree_fixture_t *fixture) {
    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture->pager);
    CHECK_OK(minidb_cursor_first(&cursor, fixture->root));

    uint64_t count = 0;
    int64_t previous_key = INT64_MIN;
    while (minidb_cursor_is_valid(&cursor)) {
        int64_t key = minidb_cursor_key(&cursor);
        CHECK(count == 0 || key > previous_key);
        previous_key = key;
        count++;
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    minidb_cursor_close(&cursor);
    return count;
}

static void test_empty_tree(void) {
    tree_fixture_t fixture = fixture_open("btree_empty");
    int64_t max_key;
    bool found;

    CHECK(scan_and_count(&fixture) == 0);
    CHECK_OK(minidb_btree_max_key(fixture.pager, fixture.root, &max_key, &found));
    CHECK(!found);
    CHECK_STATUS(MINIDB_ERR_NOT_FOUND, minidb_btree_delete(fixture.pager, fixture.root, 1));
    fixture_close(&fixture);
}

static void test_ascending_inserts_fill_leaves(void) {
    tree_fixture_t fixture = fixture_open("btree_ascending");
    enum { KEY_COUNT = 100000, PAYLOAD_LENGTH = 40 };
    uint8_t payload[PAYLOAD_LENGTH];

    for (int64_t key = 0; key < KEY_COUNT; key++) {
        make_payload(key, PAYLOAD_LENGTH, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, PAYLOAD_LENGTH));
    }
    CHECK(scan_and_count(&fixture) == KEY_COUNT);

    /* 52 bytes per cell -> 78 cells per full leaf. Splitting at the append
     * point keeps leaves full, so the tree is close to the minimum size. */
    uint32_t minimum_leaves = KEY_COUNT / 78;
    CHECK(minidb_pager_page_count(fixture.pager) < minimum_leaves + minimum_leaves / 20);

    int64_t max_key;
    bool found;
    CHECK_OK(minidb_btree_max_key(fixture.pager, fixture.root, &max_key, &found));
    CHECK(found && max_key == KEY_COUNT - 1);
    fixture_close(&fixture);
}

static void test_random_inserts_and_point_lookups(void) {
    tree_fixture_t fixture = fixture_open("btree_random");
    enum { KEY_COUNT = 50000 };
    uint8_t payload[MINIDB_BTREE_MAX_PAYLOAD];

    /* key * 7919 mod a prime visits every residue once, in scrambled order. */
    const int64_t prime = 50021;
    for (int64_t index = 0; index < KEY_COUNT; index++) {
        int64_t key = (index * 7919) % prime;
        uint32_t length = make_payload(key, (uint32_t)(key % 90), payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, length));
    }
    CHECK(scan_and_count(&fixture) == KEY_COUNT);

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture.pager);
    for (int64_t index = 0; index < KEY_COUNT; index += 7) {
        int64_t key = (index * 7919) % prime;
        CHECK_OK(minidb_cursor_seek(&cursor, fixture.root, key));
        CHECK(minidb_cursor_is_valid(&cursor) && minidb_cursor_key(&cursor) == key);
        check_payload(&cursor, key, (uint32_t)(key % 90));
    }
    minidb_cursor_close(&cursor);

    make_payload(5, 5, payload);
    CHECK_STATUS(MINIDB_ERR_DUPLICATE_KEY,
                 minidb_btree_insert(fixture.pager, fixture.root, 5, payload, 5));
    fixture_close(&fixture);
}

static void test_seek_lands_on_next_larger_key(void) {
    tree_fixture_t fixture = fixture_open("btree_seek");
    uint8_t payload[8] = {0};
    for (int64_t key = 0; key < 2000; key += 10) {
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, 8));
    }

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture.pager);
    CHECK_OK(minidb_cursor_seek(&cursor, fixture.root, 995));
    CHECK(minidb_cursor_is_valid(&cursor) && minidb_cursor_key(&cursor) == 1000);
    CHECK_OK(minidb_cursor_seek(&cursor, fixture.root, -50));
    CHECK(minidb_cursor_key(&cursor) == 0);
    CHECK_OK(minidb_cursor_seek(&cursor, fixture.root, 1991));
    CHECK(!minidb_cursor_is_valid(&cursor));
    minidb_cursor_close(&cursor);
    fixture_close(&fixture);
}

static void test_large_payloads_split_by_bytes(void) {
    tree_fixture_t fixture = fixture_open("btree_large");
    uint8_t payload[MINIDB_BTREE_MAX_PAYLOAD + 1];

    /* Mixed tiny and maximum-size payloads, inserted in descending order. */
    for (int64_t key = 600; key > 0; key--) {
        uint32_t length = key % 3 == 0 ? MINIDB_BTREE_MAX_PAYLOAD : (uint32_t)(key % 17);
        make_payload(key, length, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, length));
    }
    CHECK(scan_and_count(&fixture) == 600);

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture.pager);
    CHECK_OK(minidb_cursor_first(&cursor, fixture.root));
    for (int64_t key = 1; key <= 600; key++) {
        CHECK(minidb_cursor_key(&cursor) == key);
        check_payload(&cursor, key,
                      key % 3 == 0 ? MINIDB_BTREE_MAX_PAYLOAD : (uint32_t)(key % 17));
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    minidb_cursor_close(&cursor);

    CHECK_STATUS(MINIDB_ERR_ROW_TOO_LARGE,
                 minidb_btree_insert(fixture.pager, fixture.root, 9999, payload,
                                     MINIDB_BTREE_MAX_PAYLOAD + 1));
    fixture_close(&fixture);
}

static void test_delete_and_reinsert(void) {
    tree_fixture_t fixture = fixture_open("btree_delete");
    enum { KEY_COUNT = 20000 };
    uint8_t payload[32];

    for (int64_t key = 0; key < KEY_COUNT; key++) {
        make_payload(key, 32, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, 32));
    }
    for (int64_t key = 0; key < KEY_COUNT; key += 2) {
        CHECK_OK(minidb_btree_delete(fixture.pager, fixture.root, key));
    }
    CHECK(scan_and_count(&fixture) == KEY_COUNT / 2);
    CHECK_STATUS(MINIDB_ERR_NOT_FOUND, minidb_btree_delete(fixture.pager, fixture.root, 0));

    /* Reinserting reuses the holes: the tree must not grow. */
    uint32_t pages_before = minidb_pager_page_count(fixture.pager);
    for (int64_t key = 0; key < KEY_COUNT; key += 2) {
        make_payload(key, 32, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, 32));
    }
    CHECK(scan_and_count(&fixture) == KEY_COUNT);
    CHECK(minidb_pager_page_count(fixture.pager) == pages_before);
    fixture_close(&fixture);
}

static void test_max_key_after_trailing_deletes(void) {
    tree_fixture_t fixture = fixture_open("btree_max");
    uint8_t payload[100] = {0};
    for (int64_t key = 0; key < 5000; key++) {
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, 100));
    }
    for (int64_t key = 1000; key < 5000; key++) {
        CHECK_OK(minidb_btree_delete(fixture.pager, fixture.root, key));
    }
    int64_t max_key;
    bool found;
    CHECK_OK(minidb_btree_max_key(fixture.pager, fixture.root, &max_key, &found));
    CHECK(found && max_key == 999);
    fixture_close(&fixture);
}

static void test_update_changes_payload_size(void) {
    tree_fixture_t fixture = fixture_open("btree_update");
    enum { KEY_COUNT = 3000 };
    uint8_t payload[MINIDB_BTREE_MAX_PAYLOAD];

    for (int64_t key = 0; key < KEY_COUNT; key++) {
        make_payload(key, 20, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key, payload, 20));
    }
    /* Same size, smaller, and large enough to force leaf splits. */
    for (int64_t key = 0; key < KEY_COUNT; key++) {
        uint32_t length = key % 3 == 0 ? 20 : key % 3 == 1 ? 4 : 700;
        make_payload(key, length, payload);
        CHECK_OK(minidb_btree_update(fixture.pager, fixture.root, key, payload, length));
    }
    CHECK(scan_and_count(&fixture) == KEY_COUNT);

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture.pager);
    CHECK_OK(minidb_cursor_first(&cursor, fixture.root));
    for (int64_t key = 0; key < KEY_COUNT; key++) {
        CHECK(minidb_cursor_key(&cursor) == key);
        check_payload(&cursor, key, key % 3 == 0 ? 20 : key % 3 == 1 ? 4 : 700);
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    minidb_cursor_close(&cursor);

    CHECK_STATUS(MINIDB_ERR_NOT_FOUND,
                 minidb_btree_update(fixture.pager, fixture.root, KEY_COUNT, payload, 4));
    fixture_close(&fixture);
}

/* Random inserts, updates and deletes checked against a plain array. */
static void test_random_operations_match_a_model(void) {
    tree_fixture_t fixture = fixture_open("btree_model");
    enum { KEY_SPACE = 4000, OPERATION_COUNT = 200000 };
    static int32_t model_length[KEY_SPACE];  /* -1 = absent */
    uint8_t payload[MINIDB_BTREE_MAX_PAYLOAD];
    uint64_t live_count = 0;

    for (int key = 0; key < KEY_SPACE; key++) {
        model_length[key] = -1;
    }
    for (int operation = 0; operation < OPERATION_COUNT; operation++) {
        int64_t key = (int64_t)(next_random() % KEY_SPACE);
        uint32_t length = (uint32_t)(next_random() % 300);
        make_payload(key, length, payload);

        switch (next_random() % 3) {
            case 0: {
                minidb_status_t status =
                    minidb_btree_insert(fixture.pager, fixture.root, key, payload, length);
                if (model_length[key] >= 0) {
                    CHECK(status == MINIDB_ERR_DUPLICATE_KEY);
                } else {
                    CHECK(status == MINIDB_OK);
                    model_length[key] = (int32_t)length;
                    live_count++;
                }
                break;
            }
            case 1: {
                minidb_status_t status =
                    minidb_btree_update(fixture.pager, fixture.root, key, payload, length);
                if (model_length[key] >= 0) {
                    CHECK(status == MINIDB_OK);
                    model_length[key] = (int32_t)length;
                } else {
                    CHECK(status == MINIDB_ERR_NOT_FOUND);
                }
                break;
            }
            default: {
                minidb_status_t status = minidb_btree_delete(fixture.pager, fixture.root, key);
                if (model_length[key] >= 0) {
                    CHECK(status == MINIDB_OK);
                    model_length[key] = -1;
                    live_count--;
                } else {
                    CHECK(status == MINIDB_ERR_NOT_FOUND);
                }
                break;
            }
        }
    }

    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, fixture.pager);
    CHECK_OK(minidb_cursor_first(&cursor, fixture.root));
    uint64_t seen_count = 0;
    for (int key = 0; key < KEY_SPACE; key++) {
        if (model_length[key] < 0) {
            continue;
        }
        CHECK(minidb_cursor_is_valid(&cursor) && minidb_cursor_key(&cursor) == key);
        check_payload(&cursor, key, (uint32_t)model_length[key]);
        seen_count++;
        CHECK_OK(minidb_cursor_next(&cursor));
    }
    CHECK(!minidb_cursor_is_valid(&cursor));
    CHECK(seen_count == live_count);
    minidb_cursor_close(&cursor);
    fixture_close(&fixture);
}

static void test_tree_survives_reopen_and_destroy_frees_pages(void) {
    tree_fixture_t fixture = fixture_open("btree_reopen");
    enum { KEY_COUNT = 30000 };
    uint8_t payload[64];

    for (int64_t key = 0; key < KEY_COUNT; key++) {
        make_payload(key, 64, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key * 3, payload, 64));
    }
    CHECK_OK(minidb_pager_write_dirty(fixture.pager));
    minidb_pager_close(fixture.pager);

    CHECK_OK(minidb_pager_open(fixture.path, 64, &fixture.pager));
    CHECK(scan_and_count(&fixture) == KEY_COUNT);

    /* After destroy, a tree of the same size fits entirely in freed pages. */
    uint32_t pages_before = minidb_pager_page_count(fixture.pager);
    CHECK_OK(minidb_btree_destroy(fixture.pager, fixture.root));
    CHECK_OK(minidb_btree_create(fixture.pager, &fixture.root));
    for (int64_t key = 0; key < KEY_COUNT; key++) {
        make_payload(key, 64, payload);
        CHECK_OK(minidb_btree_insert(fixture.pager, fixture.root, key * 3, payload, 64));
    }
    CHECK(minidb_pager_page_count(fixture.pager) == pages_before);
    fixture_close(&fixture);
}

int main(void) {
    RUN(test_empty_tree);
    RUN(test_ascending_inserts_fill_leaves);
    RUN(test_random_inserts_and_point_lookups);
    RUN(test_seek_lands_on_next_larger_key);
    RUN(test_large_payloads_split_by_bytes);
    RUN(test_delete_and_reinsert);
    RUN(test_max_key_after_trailing_deletes);
    RUN(test_update_changes_payload_size);
    RUN(test_random_operations_match_a_model);
    RUN(test_tree_survives_reopen_and_destroy_frees_pages);
    return EXIT_SUCCESS;
}
