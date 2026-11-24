#include "test.h"

#include "minidb/pager.h"

static void fill_page(minidb_page_t *page, uint8_t seed) {
    for (uint32_t offset = 0; offset < MINIDB_PAGE_SIZE; offset++) {
        page->data[offset] = (uint8_t)(seed + offset);
    }
}

static bool page_matches(const minidb_page_t *page, uint8_t seed) {
    for (uint32_t offset = 0; offset < MINIDB_PAGE_SIZE; offset++) {
        if (page->data[offset] != (uint8_t)(seed + offset)) {
            return false;
        }
    }
    return true;
}

static void test_new_database_has_only_a_header(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_new");

    minidb_pager_t *pager;
    CHECK_OK(minidb_pager_open(path, 16, &pager));
    CHECK(minidb_pager_page_count(pager) == 1);
    CHECK(minidb_pager_dirty_count(pager) == 1);
    minidb_pager_close(pager);
    test_remove_database(path);
}

static void test_pages_persist_across_reopen(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_persist");

    minidb_pager_t *pager;
    CHECK_OK(minidb_pager_open(path, 16, &pager));
    for (uint8_t seed = 1; seed <= 5; seed++) {
        minidb_page_t *page;
        CHECK_OK(minidb_pager_allocate(pager, &page));
        CHECK(page->id == seed);
        fill_page(page, seed);
        minidb_pager_release(pager, page);
    }
    CHECK_OK(minidb_pager_write_dirty(pager));
    CHECK(minidb_pager_dirty_count(pager) == 0);
    minidb_pager_close(pager);

    CHECK_OK(minidb_pager_open(path, 16, &pager));
    CHECK(minidb_pager_page_count(pager) == 6);
    for (uint8_t seed = 1; seed <= 5; seed++) {
        minidb_page_t *page;
        CHECK_OK(minidb_pager_fetch(pager, seed, &page));
        CHECK(page_matches(page, seed));
        minidb_pager_release(pager, page);
    }
    minidb_page_t *missing;
    CHECK_STATUS(MINIDB_ERR_CORRUPT, minidb_pager_fetch(pager, 99, &missing));
    minidb_pager_close(pager);
    test_remove_database(path);
}

static void test_unwritten_pages_are_lost_on_close(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_discard");

    minidb_pager_t *pager;
    minidb_page_t *page;
    CHECK_OK(minidb_pager_open(path, 16, &pager));
    CHECK_OK(minidb_pager_write_dirty(pager));
    CHECK_OK(minidb_pager_allocate(pager, &page));
    minidb_pager_release(pager, page);
    minidb_pager_close(pager);  /* no write_dirty: the allocation never happened */

    CHECK_OK(minidb_pager_open(path, 16, &pager));
    CHECK(minidb_pager_page_count(pager) == 1);
    minidb_pager_close(pager);
    test_remove_database(path);
}

static void test_freed_pages_are_reused(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_free");

    minidb_pager_t *pager;
    minidb_page_t *page;
    CHECK_OK(minidb_pager_open(path, 16, &pager));
    for (int count = 0; count < 3; count++) {
        CHECK_OK(minidb_pager_allocate(pager, &page));
        minidb_pager_release(pager, page);
    }
    CHECK_OK(minidb_pager_free_page(pager, 1));
    CHECK_OK(minidb_pager_free_page(pager, 3));

    /* The free list is last-in, first-out, and reused pages come back zeroed. */
    CHECK_OK(minidb_pager_allocate(pager, &page));
    CHECK(page->id == 3 && page->data[0] == 0);
    minidb_pager_release(pager, page);
    CHECK_OK(minidb_pager_allocate(pager, &page));
    CHECK(page->id == 1);
    minidb_pager_release(pager, page);
    CHECK_OK(minidb_pager_allocate(pager, &page));
    CHECK(page->id == 4);
    minidb_pager_release(pager, page);

    minidb_pager_close(pager);
    test_remove_database(path);
}

static void test_cache_evicts_clean_pages_and_keeps_dirty_ones(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_evict");
    enum { PAGE_COUNT = 200, CACHE_CAPACITY = 16 };

    minidb_pager_t *pager;
    minidb_page_t *page;
    CHECK_OK(minidb_pager_open(path, CACHE_CAPACITY, &pager));

    /* More dirty pages than the cache holds: none may be dropped. */
    for (int id = 1; id <= PAGE_COUNT; id++) {
        CHECK_OK(minidb_pager_allocate(pager, &page));
        fill_page(page, (uint8_t)id);
        minidb_pager_release(pager, page);
    }
    CHECK(minidb_pager_dirty_count(pager) == PAGE_COUNT + 1);
    for (int id = 1; id <= PAGE_COUNT; id++) {
        CHECK_OK(minidb_pager_fetch(pager, (minidb_page_id_t)id, &page));
        CHECK(page_matches(page, (uint8_t)id));
        minidb_pager_release(pager, page);
    }
    CHECK_OK(minidb_pager_write_dirty(pager));
    minidb_pager_close(pager);

    /* Reading them all back through a small cache forces evictions. */
    CHECK_OK(minidb_pager_open(path, CACHE_CAPACITY, &pager));
    for (int pass = 0; pass < 2; pass++) {
        for (int id = 1; id <= PAGE_COUNT; id++) {
            CHECK_OK(minidb_pager_fetch(pager, (minidb_page_id_t)id, &page));
            CHECK(page_matches(page, (uint8_t)id));
            minidb_pager_release(pager, page);
        }
    }
    minidb_pager_stats_t stats = minidb_pager_stats(pager);
    CHECK(stats.cache_misses >= 2 * PAGE_COUNT - CACHE_CAPACITY);
    minidb_pager_close(pager);
    test_remove_database(path);
}

static void test_rejects_a_file_that_is_not_a_database(void) {
    char path[256];
    test_scratch_path(path, sizeof(path), "pager_garbage");

    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    char garbage[MINIDB_PAGE_SIZE];
    memset(garbage, 'x', sizeof(garbage));
    CHECK(fwrite(garbage, 1, sizeof(garbage), file) == sizeof(garbage));
    fclose(file);

    minidb_pager_t *pager;
    CHECK_STATUS(MINIDB_ERR_CORRUPT, minidb_pager_open(path, 16, &pager));
    CHECK(pager == NULL);
    test_remove_database(path);
}

int main(void) {
    RUN(test_new_database_has_only_a_header);
    RUN(test_pages_persist_across_reopen);
    RUN(test_unwritten_pages_are_lost_on_close);
    RUN(test_freed_pages_are_reused);
    RUN(test_cache_evicts_clean_pages_and_keeps_dirty_ones);
    RUN(test_rejects_a_file_that_is_not_a_database);
    return EXIT_SUCCESS;
}
