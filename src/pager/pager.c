#include "minidb/pager.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "minidb/failpoint.h"

/* ---- File header (page 0) ------------------------------------------------ */

static const uint8_t FILE_MAGIC[8] = {'M', 'I', 'N', 'I', 'D', 'B', 0, 1};

#define HEADER_MAGIC_OFFSET         0
#define HEADER_PAGE_SIZE_OFFSET     8
#define HEADER_PAGE_COUNT_OFFSET    12
#define HEADER_FREE_LIST_OFFSET     16  /* first free page, or MINIDB_NO_PAGE */

/* A free page stores the id of the next free page in its first four bytes. */
#define FREE_PAGE_NEXT_OFFSET 0

#define MINIMUM_CACHE_CAPACITY 16u

struct minidb_pager {
    int file_descriptor;
    uint32_t file_page_count;    /* pages physically present in the file */

    minidb_page_t *header_page;  /* page 0, pinned for the pager's lifetime */

    minidb_page_t **frames;      /* every cached page, in allocation order */
    uint32_t frame_count;
    uint32_t frame_slots;        /* allocated length of `frames` */
    uint32_t cache_capacity;     /* target frame_count; exceeded only if all are dirty or pinned */
    uint32_t clock_hand;         /* next frame the eviction sweep examines */
    minidb_page_t *spare_frame;  /* taken but never filled; reused first */

    minidb_page_t **buckets;     /* page table: page id -> cached page */
    uint32_t bucket_mask;        /* bucket count - 1; the count is a power of two */

    uint32_t dirty_count;
    minidb_pager_stats_t stats;
};

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

/* ---- Page table ---------------------------------------------------------- */

static uint32_t bucket_index(const minidb_pager_t *pager, minidb_page_id_t page_id) {
    return (page_id * 2654435761u) & pager->bucket_mask;
}

static minidb_page_t *page_table_find(const minidb_pager_t *pager, minidb_page_id_t page_id) {
    minidb_page_t *page = pager->buckets[bucket_index(pager, page_id)];
    while (page != NULL && page->id != page_id) {
        page = page->hash_next;
    }
    return page;
}

static void page_table_insert(minidb_pager_t *pager, minidb_page_t *page) {
    uint32_t bucket = bucket_index(pager, page->id);
    page->hash_next = pager->buckets[bucket];
    pager->buckets[bucket] = page;
}

static void page_table_remove(minidb_pager_t *pager, minidb_page_t *page) {
    minidb_page_t **link = &pager->buckets[bucket_index(pager, page->id)];
    while (*link != page) {
        link = &(*link)->hash_next;
    }
    *link = page->hash_next;
}

/* ---- Frames -------------------------------------------------------------- */

static minidb_page_t *frame_create(minidb_pager_t *pager) {
    if (pager->frame_count == pager->frame_slots) {
        uint32_t slots = pager->frame_slots * 2;
        minidb_page_t **frames = realloc(pager->frames, slots * sizeof(*frames));
        if (frames == NULL) {
            return NULL;
        }
        pager->frames = frames;
        pager->frame_slots = slots;
    }
    minidb_page_t *page = malloc(sizeof(*page));
    if (page == NULL) {
        return NULL;
    }
    pager->frames[pager->frame_count++] = page;
    return page;
}

/*
 * Clock (second-chance) sweep over clean, unpinned frames. Returns a frame
 * already removed from the page table, or NULL if every frame is in use.
 */
static minidb_page_t *frame_evict(minidb_pager_t *pager) {
    for (uint32_t step = 0; step < 2 * pager->frame_count; step++) {
        minidb_page_t *page = pager->frames[pager->clock_hand];
        pager->clock_hand = (pager->clock_hand + 1) % pager->frame_count;

        if (page->pin_count > 0 || page->is_dirty) {
            continue;
        }
        if (page->recently_used) {
            page->recently_used = false;
            continue;
        }
        page_table_remove(pager, page);
        return page;
    }
    return NULL;
}

/*
 * Returns a frame that holds no page: pinned once, and not yet in the page
 * table. The caller fills `data` and then calls frame_install(), or gives
 * the frame back with frame_return() if filling it failed.
 */
static minidb_page_t *frame_take(minidb_pager_t *pager) {
    minidb_page_t *page = pager->spare_frame;
    pager->spare_frame = NULL;

    if (page == NULL && pager->frame_count >= pager->cache_capacity) {
        page = frame_evict(pager);
    }
    if (page == NULL) {
        page = frame_create(pager);
        if (page == NULL) {
            return NULL;
        }
    }
    page->pin_count = 1;
    page->is_dirty = false;
    page->recently_used = true;
    return page;
}

static void frame_install(minidb_pager_t *pager, minidb_page_t *page, minidb_page_id_t page_id) {
    page->id = page_id;
    page_table_insert(pager, page);
}

/* Keeps an unfilled frame for the next frame_take(). It stays pinned so the
 * eviction sweep, which only removes mapped pages, skips it. */
static void frame_return(minidb_pager_t *pager, minidb_page_t *page) {
    pager->spare_frame = page;
}

/* ---- File I/O ------------------------------------------------------------ */

static minidb_status_t file_read_page(minidb_pager_t *pager, minidb_page_id_t page_id,
                                      uint8_t *out_data) {
    off_t offset = (off_t)page_id * MINIDB_PAGE_SIZE;
    ssize_t bytes_read = pread(pager->file_descriptor, out_data, MINIDB_PAGE_SIZE, offset);
    if (bytes_read < 0) {
        return MINIDB_ERR_IO;
    }
    pager->stats.pages_read++;
    return bytes_read == (ssize_t)MINIDB_PAGE_SIZE ? MINIDB_OK : MINIDB_ERR_CORRUPT;
}

static minidb_status_t file_write_page(minidb_pager_t *pager, const minidb_page_t *page) {
    off_t offset = (off_t)page->id * MINIDB_PAGE_SIZE;
    ssize_t bytes_written = pwrite(pager->file_descriptor, page->data, MINIDB_PAGE_SIZE, offset);
    if (bytes_written != (ssize_t)MINIDB_PAGE_SIZE) {
        return MINIDB_ERR_IO;
    }
    pager->stats.pages_written++;
    if (page->id >= pager->file_page_count) {
        pager->file_page_count = page->id + 1;
    }
    return MINIDB_OK;
}

/* ---- Open / close -------------------------------------------------------- */

static minidb_status_t header_create(minidb_pager_t *pager) {
    minidb_page_t *header = frame_take(pager);
    if (header == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    frame_install(pager, header, 0);
    memset(header->data, 0, MINIDB_PAGE_SIZE);
    memcpy(header->data + HEADER_MAGIC_OFFSET, FILE_MAGIC, sizeof(FILE_MAGIC));
    write_u32(header->data + HEADER_PAGE_SIZE_OFFSET, MINIDB_PAGE_SIZE);
    write_u32(header->data + HEADER_PAGE_COUNT_OFFSET, 1);
    write_u32(header->data + HEADER_FREE_LIST_OFFSET, MINIDB_NO_PAGE);
    pager->header_page = header;
    minidb_pager_mark_dirty(pager, header);
    return MINIDB_OK;
}

static minidb_status_t header_load(minidb_pager_t *pager) {
    minidb_page_t *header = frame_take(pager);
    if (header == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    minidb_status_t status = file_read_page(pager, 0, header->data);
    if (status != MINIDB_OK) {
        frame_return(pager, header);
        return status;
    }
    frame_install(pager, header, 0);
    pager->header_page = header;

    if (memcmp(header->data + HEADER_MAGIC_OFFSET, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0 ||
        read_u32(header->data + HEADER_PAGE_SIZE_OFFSET) != MINIDB_PAGE_SIZE ||
        read_u32(header->data + HEADER_PAGE_COUNT_OFFSET) > pager->file_page_count) {
        return MINIDB_ERR_CORRUPT;
    }
    return MINIDB_OK;
}

minidb_status_t minidb_pager_open(const char *path, uint32_t cache_capacity,
                                  minidb_pager_t **out_pager) {
    *out_pager = NULL;
    if (cache_capacity < MINIMUM_CACHE_CAPACITY) {
        cache_capacity = MINIMUM_CACHE_CAPACITY;
    }

    minidb_pager_t *pager = calloc(1, sizeof(*pager));
    if (pager == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    pager->file_descriptor = -1;
    pager->cache_capacity = cache_capacity;

    uint32_t bucket_count = 1;
    while (bucket_count < 2 * cache_capacity) {
        bucket_count *= 2;
    }
    pager->bucket_mask = bucket_count - 1;
    pager->buckets = calloc(bucket_count, sizeof(*pager->buckets));
    pager->frame_slots = cache_capacity;
    pager->frames = malloc(pager->frame_slots * sizeof(*pager->frames));

    minidb_status_t status = MINIDB_OK;
    struct stat file_info;
    if (pager->buckets == NULL || pager->frames == NULL) {
        status = MINIDB_ERR_NO_MEMORY;
    } else if ((pager->file_descriptor = open(path, O_RDWR | O_CREAT, 0644)) < 0 ||
               fstat(pager->file_descriptor, &file_info) != 0) {
        status = MINIDB_ERR_IO;
    } else if (file_info.st_size % MINIDB_PAGE_SIZE != 0) {
        status = MINIDB_ERR_CORRUPT;
    } else {
        pager->file_page_count = (uint32_t)(file_info.st_size / MINIDB_PAGE_SIZE);
        status = pager->file_page_count == 0 ? header_create(pager) : header_load(pager);
    }

    if (status != MINIDB_OK) {
        minidb_pager_close(pager);
        return status;
    }
    *out_pager = pager;
    return MINIDB_OK;
}

void minidb_pager_close(minidb_pager_t *pager) {
    if (pager == NULL) {
        return;
    }
    for (uint32_t index = 0; index < pager->frame_count; index++) {
        free(pager->frames[index]);
    }
    free(pager->frames);
    free(pager->buckets);
    if (pager->file_descriptor >= 0) {
        close(pager->file_descriptor);
    }
    free(pager);
}

/* ---- Page access --------------------------------------------------------- */

uint32_t minidb_pager_page_count(const minidb_pager_t *pager) {
    return read_u32(pager->header_page->data + HEADER_PAGE_COUNT_OFFSET);
}

minidb_status_t minidb_pager_fetch(minidb_pager_t *pager, minidb_page_id_t page_id,
                                   minidb_page_t **out_page) {
    *out_page = NULL;

    minidb_page_t *page = page_table_find(pager, page_id);
    if (page != NULL) {
        page->pin_count++;
        page->recently_used = true;
        pager->stats.cache_hits++;
        *out_page = page;
        return MINIDB_OK;
    }

    /*
     * A page that is not cached must be clean, so it must be in the file.
     * Anything else means a page id that was never allocated.
     */
    if (page_id >= pager->file_page_count) {
        return MINIDB_ERR_CORRUPT;
    }

    page = frame_take(pager);
    if (page == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    minidb_status_t status = file_read_page(pager, page_id, page->data);
    if (status != MINIDB_OK) {
        frame_return(pager, page);
        return status;
    }
    frame_install(pager, page, page_id);
    pager->stats.cache_misses++;
    *out_page = page;
    return MINIDB_OK;
}

void minidb_pager_release(minidb_pager_t *pager, minidb_page_t *page) {
    (void)pager;
    page->pin_count--;
}

void minidb_pager_mark_dirty(minidb_pager_t *pager, minidb_page_t *page) {
    if (!page->is_dirty) {
        page->is_dirty = true;
        pager->dirty_count++;
    }
}

minidb_status_t minidb_pager_allocate(minidb_pager_t *pager, minidb_page_t **out_page) {
    *out_page = NULL;
    uint8_t *header = pager->header_page->data;
    minidb_page_t *page;

    minidb_page_id_t free_page_id = read_u32(header + HEADER_FREE_LIST_OFFSET);
    if (free_page_id != MINIDB_NO_PAGE) {
        minidb_status_t status = minidb_pager_fetch(pager, free_page_id, &page);
        if (status != MINIDB_OK) {
            return status;
        }
        write_u32(header + HEADER_FREE_LIST_OFFSET,
                  read_u32(page->data + FREE_PAGE_NEXT_OFFSET));
    } else {
        uint32_t page_count = read_u32(header + HEADER_PAGE_COUNT_OFFSET);
        page = frame_take(pager);
        if (page == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        frame_install(pager, page, page_count);
        write_u32(header + HEADER_PAGE_COUNT_OFFSET, page_count + 1);
    }

    memset(page->data, 0, MINIDB_PAGE_SIZE);
    minidb_pager_mark_dirty(pager, page);
    minidb_pager_mark_dirty(pager, pager->header_page);
    *out_page = page;
    return MINIDB_OK;
}

minidb_status_t minidb_pager_free_page(minidb_pager_t *pager, minidb_page_id_t page_id) {
    minidb_page_t *page;
    minidb_status_t status = minidb_pager_fetch(pager, page_id, &page);
    if (status != MINIDB_OK) {
        return status;
    }
    uint8_t *header = pager->header_page->data;

    memset(page->data, 0, MINIDB_PAGE_SIZE);
    write_u32(page->data + FREE_PAGE_NEXT_OFFSET, read_u32(header + HEADER_FREE_LIST_OFFSET));
    write_u32(header + HEADER_FREE_LIST_OFFSET, page_id);

    minidb_pager_mark_dirty(pager, page);
    minidb_pager_mark_dirty(pager, pager->header_page);
    minidb_pager_release(pager, page);
    return MINIDB_OK;
}

/* ---- Checkpoint support -------------------------------------------------- */

uint32_t minidb_pager_dirty_count(const minidb_pager_t *pager) {
    return pager->dirty_count;
}

minidb_status_t minidb_pager_visit_dirty(minidb_pager_t *pager, minidb_page_visitor_t visit,
                                         void *context) {
    for (uint32_t index = 0; index < pager->frame_count; index++) {
        const minidb_page_t *page = pager->frames[index];
        if (!page->is_dirty) {
            continue;
        }
        minidb_status_t status = visit(context, page->id, page->data);
        if (status != MINIDB_OK) {
            return status;
        }
    }
    return MINIDB_OK;
}

minidb_status_t minidb_pager_write_dirty(minidb_pager_t *pager) {
    uint32_t halfway_count = pager->dirty_count / 2;

    for (uint32_t index = 0; index < pager->frame_count; index++) {
        minidb_page_t *page = pager->frames[index];
        if (!page->is_dirty) {
            continue;
        }
        if (pager->dirty_count == halfway_count) {
            minidb_failpoint_hit(MINIDB_FAILPOINT_CHECKPOINT_HALF_WRITTEN);
        }
        minidb_status_t status = file_write_page(pager, page);
        if (status != MINIDB_OK) {
            return status;
        }
        page->is_dirty = false;
        pager->dirty_count--;
    }
    return fsync(pager->file_descriptor) == 0 ? MINIDB_OK : MINIDB_ERR_IO;
}

minidb_pager_stats_t minidb_pager_stats(const minidb_pager_t *pager) {
    return pager->stats;
}
