#ifndef MINIDB_PAGER_H
#define MINIDB_PAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "minidb/status.h"

/*
 * The pager: the database file as an array of 4 KB pages behind a cache.
 *
 *   file:  [ page 0: header ][ page 1 ][ page 2 ] ...
 *
 * Callers fetch a page (which pins it in memory), read or modify its bytes,
 * mark it dirty if they changed it, and release it.
 *
 * The cache never writes a dirty page on its own ("no-steal"). Dirty pages
 * reach the file only through minidb_pager_write_dirty(), which the
 * checkpoint calls after the write-ahead log has made them recoverable. Only
 * clean, unpinned pages are evicted; if there are none the cache grows past
 * its target size rather than fail.
 *
 * Ownership: the pager owns every minidb_page_t. A pointer returned by fetch
 * or allocate is valid until the matching minidb_pager_release().
 */

#define MINIDB_PAGE_SIZE 4096u

typedef uint32_t minidb_page_id_t;

/* Page 0 is the file header, so 0 doubles as "no page" in on-disk links. */
#define MINIDB_NO_PAGE ((minidb_page_id_t)0)

typedef struct minidb_page {
    minidb_page_id_t id;
    uint8_t data[MINIDB_PAGE_SIZE];

    /* Pager bookkeeping below; other modules must not touch it. */
    uint32_t pin_count;
    bool is_dirty;
    bool recently_used;            /* second-chance bit for clock eviction */
    struct minidb_page *hash_next; /* chain within a page-table bucket */
} minidb_page_t;

typedef struct minidb_pager minidb_pager_t;

typedef struct {
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t pages_read;     /* pages read from the file */
    uint64_t pages_written;  /* pages written to the file */
} minidb_pager_stats_t;

/*
 * Opens (or creates) the database file. `cache_capacity` is the target
 * number of cached pages.
 */
minidb_status_t minidb_pager_open(const char *path, uint32_t cache_capacity,
                                  minidb_pager_t **out_pager);

/* Closes the file and frees the cache. Dirty pages are discarded. */
void minidb_pager_close(minidb_pager_t *pager);

/* Pins page `page_id` in the cache, reading it from the file on a miss. */
minidb_status_t minidb_pager_fetch(minidb_pager_t *pager, minidb_page_id_t page_id,
                                   minidb_page_t **out_page);

/* Returns a new zeroed page, pinned and dirty. Reuses freed pages first. */
minidb_status_t minidb_pager_allocate(minidb_pager_t *pager, minidb_page_t **out_page);

/* Unpins a page obtained from fetch or allocate. */
void minidb_pager_release(minidb_pager_t *pager, minidb_page_t *page);

/* Records that a pinned page's bytes were modified. */
void minidb_pager_mark_dirty(minidb_pager_t *pager, minidb_page_t *page);

/* Puts a page on the free list for a later allocate to reuse. */
minidb_status_t minidb_pager_free_page(minidb_pager_t *pager, minidb_page_id_t page_id);

/* Number of pages in the database, including the header and free pages. */
uint32_t minidb_pager_page_count(const minidb_pager_t *pager);

uint32_t minidb_pager_dirty_count(const minidb_pager_t *pager);

/* Calls `visit` once per dirty page; stops at the first non-OK status. */
typedef minidb_status_t (*minidb_page_visitor_t)(void *context, minidb_page_id_t page_id,
                                                 const uint8_t *data);
minidb_status_t minidb_pager_visit_dirty(minidb_pager_t *pager, minidb_page_visitor_t visit,
                                         void *context);

/* Writes every dirty page to the file, fsyncs it, and marks them clean. */
minidb_status_t minidb_pager_write_dirty(minidb_pager_t *pager);

minidb_pager_stats_t minidb_pager_stats(const minidb_pager_t *pager);

#endif
