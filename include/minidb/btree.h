#ifndef MINIDB_BTREE_H
#define MINIDB_BTREE_H

#include <stdbool.h>
#include <stdint.h>

#include "minidb/pager.h"
#include "minidb/status.h"

/*
 * B+-tree mapping a 64-bit integer key to a variable-length payload.
 *
 * Each table is one tree: the key is the primary key and the payload is the
 * serialized row. Internal pages only route; all payloads live in leaf pages,
 * which are chained left to right so a range scan never climbs back up.
 *
 *                      [ 40 | 90 ]                internal
 *                     /     |     \
 *          [10 20 30] -> [40 .. 80] -> [90 ..]    leaves (linked)
 *
 * A tree is identified by its root page id, which never changes: when the
 * root splits, its contents move to a new page and the root becomes their
 * parent. The catalog can therefore store the root id once.
 *
 * Deletes remove the cell but never merge pages; space is reclaimed when the
 * page is next compacted or the tree is destroyed.
 *
 * Ownership: the tree's pages belong to the pager. A cursor keeps one leaf
 * pinned while it is positioned and must be closed to unpin it.
 */

/* Largest payload a tree accepts; guarantees at least four cells per leaf. */
#define MINIDB_BTREE_MAX_PAYLOAD 1000u

/* Allocates an empty tree and returns its root page id. */
minidb_status_t minidb_btree_create(minidb_pager_t *pager, minidb_page_id_t *out_root);

/* Frees every page of the tree, including the root. */
minidb_status_t minidb_btree_destroy(minidb_pager_t *pager, minidb_page_id_t root);

/* Adds a new entry. MINIDB_ERR_DUPLICATE_KEY if the key is already present. */
minidb_status_t minidb_btree_insert(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                    const uint8_t *payload, uint32_t payload_length);

/* Replaces the payload of an existing key. MINIDB_ERR_NOT_FOUND if absent. */
minidb_status_t minidb_btree_update(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                    const uint8_t *payload, uint32_t payload_length);

/* Removes an entry. MINIDB_ERR_NOT_FOUND if absent. */
minidb_status_t minidb_btree_delete(minidb_pager_t *pager, minidb_page_id_t root, int64_t key);

/* Largest key in the tree; *out_found is false when the tree is empty. */
minidb_status_t minidb_btree_max_key(minidb_pager_t *pager, minidb_page_id_t root,
                                     int64_t *out_key, bool *out_found);

/* ---- Cursor: ordered iteration over the leaves --------------------------- */

typedef struct {
    minidb_pager_t *pager;
    minidb_page_t *leaf;  /* pinned leaf the cursor is on, or NULL at the end */
    uint16_t slot;        /* index of the current cell within `leaf` */
} minidb_cursor_t;

void minidb_cursor_init(minidb_cursor_t *cursor, minidb_pager_t *pager);

/* Positions the cursor on the first entry whose key is >= `key`. */
minidb_status_t minidb_cursor_seek(minidb_cursor_t *cursor, minidb_page_id_t root, int64_t key);

/* Positions the cursor on the smallest key. */
minidb_status_t minidb_cursor_first(minidb_cursor_t *cursor, minidb_page_id_t root);

/* False once the cursor has moved past the last entry. */
bool minidb_cursor_is_valid(const minidb_cursor_t *cursor);

minidb_status_t minidb_cursor_next(minidb_cursor_t *cursor);

/* Key and payload of the current entry. The payload points into the pinned
 * leaf and is valid until the cursor moves or closes. */
int64_t minidb_cursor_key(const minidb_cursor_t *cursor);
const uint8_t *minidb_cursor_payload(const minidb_cursor_t *cursor, uint32_t *out_length);

/* Unpins the current leaf. Safe to call more than once. */
void minidb_cursor_close(minidb_cursor_t *cursor);

#endif
