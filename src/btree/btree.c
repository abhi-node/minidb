#include "minidb/btree.h"

#include <string.h>

/*
 * Node layout (one node per 4 KB page).
 *
 * Common header, 12 bytes:
 *
 *   0  u8   node type (leaf or internal)
 *   2  u16  cell count
 *   4  u16  content start      leaf only: lowest byte used by cell bodies
 *   6  u16  fragmented bytes   leaf only: holes left by removed cells
 *   8  u32  right link         leaf: next leaf; internal: rightmost child
 *
 * Leaf: a slot array grows up from the header, cell bodies grow down from
 * the end of the page, and the free space is the gap between them.
 *
 *   [ header | slot 0 | slot 1 | ... ->   free   <- ... | cell | cell ]
 *
 *   slot:  u16 offset of the cell body, slots kept sorted by key
 *   cell:  i64 key, u16 payload length, payload bytes
 *
 * Internal: a sorted array of fixed-size entries.
 *
 *   entry: i64 key, u32 child   -- child holds keys strictly below key
 *   right link                  -- holds keys >= the last entry's key
 */

#define NODE_TYPE_OFFSET          0
#define NODE_CELL_COUNT_OFFSET    2
#define NODE_CONTENT_START_OFFSET 4
#define NODE_FRAGMENTED_OFFSET    6
#define NODE_RIGHT_LINK_OFFSET    8
#define NODE_HEADER_SIZE          12u

enum { NODE_TYPE_LEAF = 1, NODE_TYPE_INTERNAL = 2 };

#define LEAF_SLOT_SIZE        2u
#define LEAF_CELL_HEADER_SIZE 10u  /* key + payload length */
#define LEAF_MAX_CELLS \
    ((MINIDB_PAGE_SIZE - NODE_HEADER_SIZE) / (LEAF_SLOT_SIZE + LEAF_CELL_HEADER_SIZE))

#define INTERNAL_ENTRY_SIZE  12u  /* key + child page id */
#define INTERNAL_MAX_ENTRIES ((MINIDB_PAGE_SIZE - NODE_HEADER_SIZE) / INTERNAL_ENTRY_SIZE)

/* Far deeper than a tree of 2^32 pages can get; exceeding it means a cycle. */
#define MAX_TREE_HEIGHT 32

/* One step of a root-to-leaf descent: which child of which page was taken. */
typedef struct {
    minidb_page_id_t page_id;
    uint16_t child_index;
} path_step_t;

/* ---- Unaligned field access ---------------------------------------------- */

static uint16_t read_u16(const uint8_t *bytes) {
    uint16_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static int64_t read_i64(const uint8_t *bytes) {
    int64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u16(uint8_t *bytes, uint16_t value) { memcpy(bytes, &value, sizeof(value)); }
static void write_u32(uint8_t *bytes, uint32_t value) { memcpy(bytes, &value, sizeof(value)); }
static void write_i64(uint8_t *bytes, int64_t value) { memcpy(bytes, &value, sizeof(value)); }

/* ---- Fields common to both node types ------------------------------------ */

static uint8_t node_type(const uint8_t *node) { return node[NODE_TYPE_OFFSET]; }

static uint16_t node_cell_count(const uint8_t *node) {
    return read_u16(node + NODE_CELL_COUNT_OFFSET);
}

static void node_set_cell_count(uint8_t *node, uint16_t count) {
    write_u16(node + NODE_CELL_COUNT_OFFSET, count);
}

static minidb_page_id_t node_right_link(const uint8_t *node) {
    return read_u32(node + NODE_RIGHT_LINK_OFFSET);
}

static void node_set_right_link(uint8_t *node, minidb_page_id_t page_id) {
    write_u32(node + NODE_RIGHT_LINK_OFFSET, page_id);
}

static void node_init(uint8_t *node, uint8_t type) {
    memset(node, 0, NODE_HEADER_SIZE);
    node[NODE_TYPE_OFFSET] = type;
    write_u16(node + NODE_CONTENT_START_OFFSET, (uint16_t)MINIDB_PAGE_SIZE);
}

/* ---- Leaf nodes ---------------------------------------------------------- */

static uint16_t leaf_content_start(const uint8_t *leaf) {
    return read_u16(leaf + NODE_CONTENT_START_OFFSET);
}

static uint16_t leaf_fragmented_bytes(const uint8_t *leaf) {
    return read_u16(leaf + NODE_FRAGMENTED_OFFSET);
}

static uint8_t *leaf_slot(const uint8_t *leaf, uint16_t slot) {
    return (uint8_t *)leaf + NODE_HEADER_SIZE + slot * LEAF_SLOT_SIZE;
}

static uint8_t *leaf_cell(const uint8_t *leaf, uint16_t slot) {
    return (uint8_t *)leaf + read_u16(leaf_slot(leaf, slot));
}

static int64_t leaf_key(const uint8_t *leaf, uint16_t slot) {
    return read_i64(leaf_cell(leaf, slot));
}

static uint16_t leaf_payload_length(const uint8_t *leaf, uint16_t slot) {
    return read_u16(leaf_cell(leaf, slot) + sizeof(int64_t));
}

static uint8_t *leaf_payload(const uint8_t *leaf, uint16_t slot) {
    return leaf_cell(leaf, slot) + LEAF_CELL_HEADER_SIZE;
}

/* Page bytes one cell occupies: its slot plus its body. */
static uint32_t leaf_cell_footprint(uint32_t payload_length) {
    return LEAF_SLOT_SIZE + LEAF_CELL_HEADER_SIZE + payload_length;
}

/* Bytes between the end of the slot array and the first cell body. */
static uint32_t leaf_contiguous_free(const uint8_t *leaf) {
    uint32_t slots_end = NODE_HEADER_SIZE + node_cell_count(leaf) * LEAF_SLOT_SIZE;
    return leaf_content_start(leaf) - slots_end;
}

/* Index of the first cell whose key is >= `key` (the cell count if none). */
static uint16_t leaf_lower_bound(const uint8_t *leaf, int64_t key) {
    uint16_t low = 0;
    uint16_t high = node_cell_count(leaf);
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2);
        if (leaf_key(leaf, middle) < key) {
            low = (uint16_t)(middle + 1);
        } else {
            high = middle;
        }
    }
    return low;
}

/* Inserts a cell at `slot`. The caller guarantees enough contiguous space. */
static void leaf_insert_cell(uint8_t *leaf, uint16_t slot, int64_t key, const uint8_t *payload,
                             uint32_t payload_length) {
    uint16_t cell_count = node_cell_count(leaf);
    uint16_t cell_offset =
        (uint16_t)(leaf_content_start(leaf) - LEAF_CELL_HEADER_SIZE - payload_length);

    uint8_t *cell = leaf + cell_offset;
    write_i64(cell, key);
    write_u16(cell + sizeof(int64_t), (uint16_t)payload_length);
    memcpy(cell + LEAF_CELL_HEADER_SIZE, payload, payload_length);

    uint8_t *slot_bytes = leaf_slot(leaf, slot);
    memmove(slot_bytes + LEAF_SLOT_SIZE, slot_bytes, (cell_count - slot) * LEAF_SLOT_SIZE);
    write_u16(slot_bytes, cell_offset);

    write_u16(leaf + NODE_CONTENT_START_OFFSET, cell_offset);
    node_set_cell_count(leaf, (uint16_t)(cell_count + 1));
}

/* Removes the cell at `slot`; its body becomes a hole until compaction. */
static void leaf_remove_cell(uint8_t *leaf, uint16_t slot) {
    uint16_t cell_count = node_cell_count(leaf);
    uint16_t cell_offset = read_u16(leaf_slot(leaf, slot));
    uint16_t body_size = (uint16_t)(LEAF_CELL_HEADER_SIZE + leaf_payload_length(leaf, slot));

    if (cell_offset == leaf_content_start(leaf)) {
        /* The lowest body: hand its bytes straight back to the free gap. */
        write_u16(leaf + NODE_CONTENT_START_OFFSET, (uint16_t)(cell_offset + body_size));
    } else {
        write_u16(leaf + NODE_FRAGMENTED_OFFSET,
                  (uint16_t)(leaf_fragmented_bytes(leaf) + body_size));
    }

    uint8_t *slot_bytes = leaf_slot(leaf, slot);
    memmove(slot_bytes, slot_bytes + LEAF_SLOT_SIZE, (cell_count - slot - 1u) * LEAF_SLOT_SIZE);
    node_set_cell_count(leaf, (uint16_t)(cell_count - 1));
}

/* Rewrites the leaf with its cell bodies packed, turning holes into free space. */
static void leaf_compact(uint8_t *leaf) {
    uint8_t original[MINIDB_PAGE_SIZE];
    memcpy(original, leaf, MINIDB_PAGE_SIZE);

    minidb_page_id_t next_leaf = node_right_link(original);
    uint16_t cell_count = node_cell_count(original);

    node_init(leaf, NODE_TYPE_LEAF);
    node_set_right_link(leaf, next_leaf);
    for (uint16_t slot = 0; slot < cell_count; slot++) {
        leaf_insert_cell(leaf, slot, leaf_key(original, slot), leaf_payload(original, slot),
                         leaf_payload_length(original, slot));
    }
}

/* True if a cell of this size fits, compacting the leaf first if it must. */
static bool leaf_make_room(uint8_t *leaf, uint32_t payload_length) {
    uint32_t needed = leaf_cell_footprint(payload_length);
    if (leaf_contiguous_free(leaf) >= needed) {
        return true;
    }
    if (leaf_contiguous_free(leaf) + leaf_fragmented_bytes(leaf) < needed) {
        return false;
    }
    leaf_compact(leaf);
    return true;
}

/* A cell by reference, used while redistributing cells during a split. */
typedef struct {
    int64_t key;
    const uint8_t *payload;
    uint16_t payload_length;
} leaf_cell_ref_t;

/*
 * Splits a full leaf while inserting one new cell at `slot`.
 *
 * The lower cells stay in `left_page`, the upper cells move to a new right
 * sibling, and *out_separator is the right sibling's first key.
 *
 * Normally the cells are divided evenly by bytes. When the new cell is being
 * appended to the last leaf of the tree, the split is instead made right
 * before it: ascending inserts then leave every leaf full instead of half
 * empty.
 */
static minidb_status_t leaf_split_insert(minidb_pager_t *pager, minidb_page_t *left_page,
                                         uint16_t slot, int64_t key, const uint8_t *payload,
                                         uint32_t payload_length, int64_t *out_separator,
                                         minidb_page_id_t *out_right) {
    minidb_page_t *right_page;
    minidb_status_t status = minidb_pager_allocate(pager, &right_page);
    if (status != MINIDB_OK) {
        return status;
    }

    uint8_t original[MINIDB_PAGE_SIZE];
    memcpy(original, left_page->data, MINIDB_PAGE_SIZE);
    uint16_t original_count = node_cell_count(original);
    minidb_page_id_t original_next_leaf = node_right_link(original);

    /* All cells in key order, with the new one in place. */
    leaf_cell_ref_t cells[LEAF_MAX_CELLS + 1];
    uint16_t total_count = (uint16_t)(original_count + 1);
    uint32_t total_bytes = 0;
    for (uint16_t index = 0; index < total_count; index++) {
        if (index == slot) {
            cells[index] = (leaf_cell_ref_t){key, payload, (uint16_t)payload_length};
        } else {
            uint16_t source = index < slot ? index : (uint16_t)(index - 1);
            cells[index] = (leaf_cell_ref_t){leaf_key(original, source),
                                             leaf_payload(original, source),
                                             leaf_payload_length(original, source)};
        }
        total_bytes += leaf_cell_footprint(cells[index].payload_length);
    }

    /* left_count cells stay in the left leaf. */
    uint16_t left_count;
    bool appending_to_last_leaf = slot == original_count && original_next_leaf == MINIDB_NO_PAGE;
    if (appending_to_last_leaf) {
        left_count = original_count;
    } else {
        uint32_t left_bytes = 0;
        left_count = 0;
        while (left_count < total_count - 1 && left_bytes < total_bytes / 2) {
            left_bytes += leaf_cell_footprint(cells[left_count].payload_length);
            left_count++;
        }
    }

    uint8_t *left = left_page->data;
    uint8_t *right = right_page->data;

    node_init(left, NODE_TYPE_LEAF);
    for (uint16_t index = 0; index < left_count; index++) {
        leaf_insert_cell(left, index, cells[index].key, cells[index].payload,
                         cells[index].payload_length);
    }
    node_init(right, NODE_TYPE_LEAF);
    for (uint16_t index = left_count; index < total_count; index++) {
        leaf_insert_cell(right, (uint16_t)(index - left_count), cells[index].key,
                         cells[index].payload, cells[index].payload_length);
    }

    /* Splice the new leaf into the chain: left -> right -> old next. */
    node_set_right_link(right, original_next_leaf);
    node_set_right_link(left, right_page->id);

    *out_separator = cells[left_count].key;
    *out_right = right_page->id;

    minidb_pager_mark_dirty(pager, left_page);
    minidb_pager_mark_dirty(pager, right_page);
    minidb_pager_release(pager, right_page);
    return MINIDB_OK;
}

/* ---- Internal nodes ------------------------------------------------------ */

static uint8_t *internal_entry(const uint8_t *node, uint16_t index) {
    return (uint8_t *)node + NODE_HEADER_SIZE + index * INTERNAL_ENTRY_SIZE;
}

static int64_t internal_key(const uint8_t *node, uint16_t index) {
    return read_i64(internal_entry(node, index));
}

/* Child `index` of the node; index == cell count selects the right link. */
static minidb_page_id_t internal_child(const uint8_t *node, uint16_t index) {
    if (index == node_cell_count(node)) {
        return node_right_link(node);
    }
    return read_u32(internal_entry(node, index) + sizeof(int64_t));
}

static void internal_set_child(uint8_t *node, uint16_t index, minidb_page_id_t child) {
    if (index == node_cell_count(node)) {
        node_set_right_link(node, child);
    } else {
        write_u32(internal_entry(node, index) + sizeof(int64_t), child);
    }
}

/* Index of the child whose subtree covers `key`. */
static uint16_t internal_find_child(const uint8_t *node, int64_t key) {
    uint16_t low = 0;
    uint16_t high = node_cell_count(node);
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2);
        if (key < internal_key(node, middle)) {
            high = middle;
        } else {
            low = (uint16_t)(middle + 1);
        }
    }
    return low;
}

static void internal_append_entry(uint8_t *node, int64_t key, minidb_page_id_t child) {
    uint16_t cell_count = node_cell_count(node);
    uint8_t *entry = internal_entry(node, cell_count);
    write_i64(entry, key);
    write_u32(entry + sizeof(int64_t), child);
    node_set_cell_count(node, (uint16_t)(cell_count + 1));
}

/*
 * Records that the child at `child_index` split: it keeps keys below
 * `separator`, and `right_child` takes the rest. The node must have room.
 */
static void internal_insert_split(uint8_t *node, uint16_t child_index, int64_t separator,
                                  minidb_page_id_t right_child) {
    uint16_t cell_count = node_cell_count(node);
    minidb_page_id_t left_child = internal_child(node, child_index);

    uint8_t *entry = internal_entry(node, child_index);
    memmove(entry + INTERNAL_ENTRY_SIZE, entry, (cell_count - child_index) * INTERNAL_ENTRY_SIZE);
    write_i64(entry, separator);
    write_u32(entry + sizeof(int64_t), left_child);
    node_set_cell_count(node, (uint16_t)(cell_count + 1));

    /* The slot that used to route to the split child now routes to its sibling. */
    internal_set_child(node, (uint16_t)(child_index + 1), right_child);
}

/*
 * Splits a full internal node while recording a child split (see
 * internal_insert_split). The middle key moves up as *out_separator.
 */
static minidb_status_t internal_split_insert(minidb_pager_t *pager, minidb_page_t *left_page,
                                             uint16_t child_index, int64_t separator,
                                             minidb_page_id_t right_child,
                                             int64_t *out_separator,
                                             minidb_page_id_t *out_right) {
    minidb_page_t *right_page;
    minidb_status_t status = minidb_pager_allocate(pager, &right_page);
    if (status != MINIDB_OK) {
        return status;
    }
    uint8_t *left = left_page->data;
    uint8_t *right = right_page->data;

    /* Every key and child, in order, with the new separator in place. */
    int64_t keys[INTERNAL_MAX_ENTRIES + 1];
    minidb_page_id_t children[INTERNAL_MAX_ENTRIES + 2];
    uint16_t original_count = node_cell_count(left);
    uint16_t key_count = 0;
    for (uint16_t index = 0; index <= original_count; index++) {
        children[key_count] = internal_child(left, index);
        if (index == child_index) {
            keys[key_count++] = separator;
            children[key_count] = right_child;
        }
        if (index < original_count) {
            keys[key_count++] = internal_key(left, index);
        }
    }

    uint16_t middle = (uint16_t)(key_count / 2);

    node_init(left, NODE_TYPE_INTERNAL);
    for (uint16_t index = 0; index < middle; index++) {
        internal_append_entry(left, keys[index], children[index]);
    }
    node_set_right_link(left, children[middle]);

    node_init(right, NODE_TYPE_INTERNAL);
    for (uint16_t index = (uint16_t)(middle + 1); index < key_count; index++) {
        internal_append_entry(right, keys[index], children[index]);
    }
    node_set_right_link(right, children[key_count]);

    *out_separator = keys[middle];
    *out_right = right_page->id;

    minidb_pager_mark_dirty(pager, left_page);
    minidb_pager_mark_dirty(pager, right_page);
    minidb_pager_release(pager, right_page);
    return MINIDB_OK;
}

/* ---- Descent and split propagation --------------------------------------- */

/*
 * Walks from the root to the leaf that covers `key` and returns it pinned.
 * If `path` is not NULL it receives one step per internal node visited.
 */
static minidb_status_t descend_to_leaf(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                       path_step_t *path, int *out_depth,
                                       minidb_page_t **out_leaf) {
    minidb_page_t *page;
    minidb_status_t status = minidb_pager_fetch(pager, root, &page);
    int depth = 0;

    while (status == MINIDB_OK && node_type(page->data) == NODE_TYPE_INTERNAL) {
        if (depth == MAX_TREE_HEIGHT) {
            status = MINIDB_ERR_CORRUPT;
            break;
        }
        uint16_t child_index = internal_find_child(page->data, key);
        minidb_page_id_t child = internal_child(page->data, child_index);
        if (path != NULL) {
            path[depth] = (path_step_t){page->id, child_index};
        }
        depth++;
        minidb_pager_release(pager, page);
        status = minidb_pager_fetch(pager, child, &page);
    }

    if (status == MINIDB_OK && node_type(page->data) != NODE_TYPE_LEAF) {
        status = MINIDB_ERR_CORRUPT;
    }
    if (status != MINIDB_OK) {
        if (page != NULL) {
            minidb_pager_release(pager, page);
        }
        return status;
    }
    if (out_depth != NULL) {
        *out_depth = depth;
    }
    *out_leaf = page;
    return MINIDB_OK;
}

/*
 * The root split into itself (lower half) and `right_child`. The root page
 * id must not change, so move the lower half to a new page and turn the root
 * into an internal node with those two children.
 */
static minidb_status_t root_grow(minidb_pager_t *pager, minidb_page_id_t root, int64_t separator,
                                 minidb_page_id_t right_child) {
    minidb_page_t *root_page;
    minidb_page_t *left_page;
    minidb_status_t status = minidb_pager_fetch(pager, root, &root_page);
    if (status != MINIDB_OK) {
        return status;
    }
    status = minidb_pager_allocate(pager, &left_page);
    if (status != MINIDB_OK) {
        minidb_pager_release(pager, root_page);
        return status;
    }

    memcpy(left_page->data, root_page->data, MINIDB_PAGE_SIZE);

    node_init(root_page->data, NODE_TYPE_INTERNAL);
    internal_append_entry(root_page->data, separator, left_page->id);
    node_set_right_link(root_page->data, right_child);

    minidb_pager_mark_dirty(pager, root_page);
    minidb_pager_mark_dirty(pager, left_page);
    minidb_pager_release(pager, left_page);
    minidb_pager_release(pager, root_page);
    return MINIDB_OK;
}

/*
 * A node at the bottom of `path` split, producing `separator` and
 * `right_child`. Records the split in its parent, splitting upward for as
 * long as the parents are full.
 */
static minidb_status_t propagate_split(minidb_pager_t *pager, minidb_page_id_t root,
                                       const path_step_t *path, int depth, int64_t separator,
                                       minidb_page_id_t right_child) {
    for (int level = depth - 1; level >= 0; level--) {
        minidb_page_t *parent;
        minidb_status_t status = minidb_pager_fetch(pager, path[level].page_id, &parent);
        if (status != MINIDB_OK) {
            return status;
        }

        if (node_cell_count(parent->data) < INTERNAL_MAX_ENTRIES) {
            internal_insert_split(parent->data, path[level].child_index, separator, right_child);
            minidb_pager_mark_dirty(pager, parent);
            minidb_pager_release(pager, parent);
            return MINIDB_OK;
        }

        status = internal_split_insert(pager, parent, path[level].child_index, separator,
                                       right_child, &separator, &right_child);
        minidb_pager_release(pager, parent);
        if (status != MINIDB_OK) {
            return status;
        }
    }
    return root_grow(pager, root, separator, right_child);
}

/* ---- Public operations --------------------------------------------------- */

minidb_status_t minidb_btree_create(minidb_pager_t *pager, minidb_page_id_t *out_root) {
    minidb_page_t *root_page;
    minidb_status_t status = minidb_pager_allocate(pager, &root_page);
    if (status != MINIDB_OK) {
        return status;
    }
    node_init(root_page->data, NODE_TYPE_LEAF);
    *out_root = root_page->id;
    minidb_pager_release(pager, root_page);
    return MINIDB_OK;
}

static minidb_status_t destroy_subtree(minidb_pager_t *pager, minidb_page_id_t page_id,
                                       int depth) {
    if (depth > MAX_TREE_HEIGHT) {
        return MINIDB_ERR_CORRUPT;
    }
    minidb_page_t *page;
    minidb_status_t status = minidb_pager_fetch(pager, page_id, &page);
    if (status != MINIDB_OK) {
        return status;
    }

    /* Copy the child ids out so the page is not pinned across the recursion. */
    minidb_page_id_t children[INTERNAL_MAX_ENTRIES + 1];
    uint16_t child_count = 0;
    if (node_type(page->data) == NODE_TYPE_INTERNAL) {
        child_count = (uint16_t)(node_cell_count(page->data) + 1);
        for (uint16_t index = 0; index < child_count; index++) {
            children[index] = internal_child(page->data, index);
        }
    }
    minidb_pager_release(pager, page);

    for (uint16_t index = 0; index < child_count; index++) {
        status = destroy_subtree(pager, children[index], depth + 1);
        if (status != MINIDB_OK) {
            return status;
        }
    }
    return minidb_pager_free_page(pager, page_id);
}

minidb_status_t minidb_btree_destroy(minidb_pager_t *pager, minidb_page_id_t root) {
    return destroy_subtree(pager, root, 0);
}

minidb_status_t minidb_btree_insert(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                    const uint8_t *payload, uint32_t payload_length) {
    if (payload_length > MINIDB_BTREE_MAX_PAYLOAD) {
        return MINIDB_ERR_ROW_TOO_LARGE;
    }

    path_step_t path[MAX_TREE_HEIGHT];
    int depth;
    minidb_page_t *leaf_page;
    minidb_status_t status = descend_to_leaf(pager, root, key, path, &depth, &leaf_page);
    if (status != MINIDB_OK) {
        return status;
    }
    uint8_t *leaf = leaf_page->data;

    uint16_t slot = leaf_lower_bound(leaf, key);
    if (slot < node_cell_count(leaf) && leaf_key(leaf, slot) == key) {
        minidb_pager_release(pager, leaf_page);
        return MINIDB_ERR_DUPLICATE_KEY;
    }

    if (leaf_make_room(leaf, payload_length)) {
        leaf_insert_cell(leaf, slot, key, payload, payload_length);
        minidb_pager_mark_dirty(pager, leaf_page);
        minidb_pager_release(pager, leaf_page);
        return MINIDB_OK;
    }

    int64_t separator;
    minidb_page_id_t right_child;
    status = leaf_split_insert(pager, leaf_page, slot, key, payload, payload_length, &separator,
                               &right_child);
    minidb_pager_release(pager, leaf_page);
    if (status != MINIDB_OK) {
        return status;
    }
    return propagate_split(pager, root, path, depth, separator, right_child);
}

/*
 * Pins the leaf holding `key` and reports its slot, or returns
 * MINIDB_ERR_NOT_FOUND with nothing pinned.
 */
static minidb_status_t find_cell(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                 minidb_page_t **out_leaf, uint16_t *out_slot) {
    minidb_page_t *leaf_page;
    minidb_status_t status = descend_to_leaf(pager, root, key, NULL, NULL, &leaf_page);
    if (status != MINIDB_OK) {
        return status;
    }
    uint16_t slot = leaf_lower_bound(leaf_page->data, key);
    if (slot == node_cell_count(leaf_page->data) || leaf_key(leaf_page->data, slot) != key) {
        minidb_pager_release(pager, leaf_page);
        return MINIDB_ERR_NOT_FOUND;
    }
    *out_leaf = leaf_page;
    *out_slot = slot;
    return MINIDB_OK;
}

minidb_status_t minidb_btree_update(minidb_pager_t *pager, minidb_page_id_t root, int64_t key,
                                    const uint8_t *payload, uint32_t payload_length) {
    if (payload_length > MINIDB_BTREE_MAX_PAYLOAD) {
        return MINIDB_ERR_ROW_TOO_LARGE;
    }

    minidb_page_t *leaf_page;
    uint16_t slot;
    minidb_status_t status = find_cell(pager, root, key, &leaf_page, &slot);
    if (status != MINIDB_OK) {
        return status;
    }
    uint8_t *leaf = leaf_page->data;
    minidb_pager_mark_dirty(pager, leaf_page);

    if (leaf_payload_length(leaf, slot) == payload_length) {
        memcpy(leaf_payload(leaf, slot), payload, payload_length);
        minidb_pager_release(pager, leaf_page);
        return MINIDB_OK;
    }

    /* The size changed: drop the old cell and insert the new one in its place. */
    leaf_remove_cell(leaf, slot);
    if (leaf_make_room(leaf, payload_length)) {
        leaf_insert_cell(leaf, slot, key, payload, payload_length);
        minidb_pager_release(pager, leaf_page);
        return MINIDB_OK;
    }

    /* It no longer fits in this leaf; the general insert path will split it. */
    minidb_pager_release(pager, leaf_page);
    return minidb_btree_insert(pager, root, key, payload, payload_length);
}

minidb_status_t minidb_btree_delete(minidb_pager_t *pager, minidb_page_id_t root, int64_t key) {
    minidb_page_t *leaf_page;
    uint16_t slot;
    minidb_status_t status = find_cell(pager, root, key, &leaf_page, &slot);
    if (status != MINIDB_OK) {
        return status;
    }
    leaf_remove_cell(leaf_page->data, slot);
    minidb_pager_mark_dirty(pager, leaf_page);
    minidb_pager_release(pager, leaf_page);
    return MINIDB_OK;
}

minidb_status_t minidb_btree_max_key(minidb_pager_t *pager, minidb_page_id_t root,
                                     int64_t *out_key, bool *out_found) {
    *out_found = false;

    minidb_page_t *leaf_page;
    minidb_status_t status = descend_to_leaf(pager, root, INT64_MAX, NULL, NULL, &leaf_page);
    if (status != MINIDB_OK) {
        return status;
    }
    uint16_t cell_count = node_cell_count(leaf_page->data);
    if (cell_count > 0) {
        *out_key = leaf_key(leaf_page->data, (uint16_t)(cell_count - 1));
        *out_found = true;
    }
    minidb_pager_release(pager, leaf_page);
    if (*out_found) {
        return MINIDB_OK;
    }

    /* Deletes emptied the last leaf (leaves are never merged): walk the chain. */
    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, pager);
    status = minidb_cursor_first(&cursor, root);
    while (status == MINIDB_OK && minidb_cursor_is_valid(&cursor)) {
        *out_key = minidb_cursor_key(&cursor);
        *out_found = true;
        status = minidb_cursor_next(&cursor);
    }
    minidb_cursor_close(&cursor);
    return status;
}

/* ---- Cursor -------------------------------------------------------------- */

void minidb_cursor_init(minidb_cursor_t *cursor, minidb_pager_t *pager) {
    cursor->pager = pager;
    cursor->leaf = NULL;
    cursor->slot = 0;
}

/* Moves past exhausted (or empty) leaves until the cursor is on a cell. */
static minidb_status_t cursor_skip_exhausted_leaves(minidb_cursor_t *cursor) {
    while (cursor->leaf != NULL && cursor->slot >= node_cell_count(cursor->leaf->data)) {
        minidb_page_id_t next_leaf = node_right_link(cursor->leaf->data);
        minidb_pager_release(cursor->pager, cursor->leaf);
        cursor->leaf = NULL;
        cursor->slot = 0;

        if (next_leaf == MINIDB_NO_PAGE) {
            break;
        }
        minidb_status_t status = minidb_pager_fetch(cursor->pager, next_leaf, &cursor->leaf);
        if (status != MINIDB_OK) {
            return status;
        }
        if (node_type(cursor->leaf->data) != NODE_TYPE_LEAF) {
            minidb_cursor_close(cursor);
            return MINIDB_ERR_CORRUPT;
        }
    }
    return MINIDB_OK;
}

minidb_status_t minidb_cursor_seek(minidb_cursor_t *cursor, minidb_page_id_t root, int64_t key) {
    minidb_cursor_close(cursor);
    minidb_status_t status = descend_to_leaf(cursor->pager, root, key, NULL, NULL, &cursor->leaf);
    if (status != MINIDB_OK) {
        cursor->leaf = NULL;
        return status;
    }
    cursor->slot = leaf_lower_bound(cursor->leaf->data, key);
    return cursor_skip_exhausted_leaves(cursor);
}

minidb_status_t minidb_cursor_first(minidb_cursor_t *cursor, minidb_page_id_t root) {
    return minidb_cursor_seek(cursor, root, INT64_MIN);
}

bool minidb_cursor_is_valid(const minidb_cursor_t *cursor) {
    return cursor->leaf != NULL;
}

minidb_status_t minidb_cursor_next(minidb_cursor_t *cursor) {
    cursor->slot++;
    return cursor_skip_exhausted_leaves(cursor);
}

int64_t minidb_cursor_key(const minidb_cursor_t *cursor) {
    return leaf_key(cursor->leaf->data, cursor->slot);
}

const uint8_t *minidb_cursor_payload(const minidb_cursor_t *cursor, uint32_t *out_length) {
    *out_length = leaf_payload_length(cursor->leaf->data, cursor->slot);
    return leaf_payload(cursor->leaf->data, cursor->slot);
}

void minidb_cursor_close(minidb_cursor_t *cursor) {
    if (cursor->leaf != NULL) {
        minidb_pager_release(cursor->pager, cursor->leaf);
        cursor->leaf = NULL;
    }
    cursor->slot = 0;
}
