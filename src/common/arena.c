#include "minidb/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE 16384u
#define ARENA_ALIGNMENT  sizeof(max_align_t)

struct minidb_arena_block {
    minidb_arena_block_t *next;
    size_t capacity;  /* usable bytes in `bytes` */
    size_t used;
    max_align_t bytes[];  /* flexible array keeps the payload max-aligned */
};

static size_t round_up_to_alignment(size_t size) {
    return (size + ARENA_ALIGNMENT - 1) & ~(ARENA_ALIGNMENT - 1);
}

static minidb_arena_block_t *block_create(size_t capacity) {
    minidb_arena_block_t *block = malloc(sizeof(*block) + capacity);
    if (block == NULL) {
        return NULL;
    }
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

void minidb_arena_init(minidb_arena_t *arena) {
    arena->current = NULL;
    arena->first = NULL;
}

void *minidb_arena_alloc(minidb_arena_t *arena, size_t size) {
    size = round_up_to_alignment(size == 0 ? 1 : size);

    minidb_arena_block_t *block = arena->current;
    if (block == NULL || block->capacity - block->used < size) {
        /* Reuse the next block left over from before a reset, if it fits. */
        minidb_arena_block_t *next = block != NULL ? block->next : arena->first;
        if (next == NULL || next->capacity < size) {
            size_t capacity = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
            minidb_arena_block_t *created = block_create(capacity);
            if (created == NULL) {
                return NULL;
            }
            created->next = next;
            if (block != NULL) {
                block->next = created;
            } else {
                arena->first = created;
            }
            next = created;
        }
        next->used = 0;
        arena->current = next;
        block = next;
    }

    void *result = (uint8_t *)block->bytes + block->used;
    block->used += size;
    return result;
}

void *minidb_arena_alloc_zeroed(minidb_arena_t *arena, size_t size) {
    void *result = minidb_arena_alloc(arena, size);
    if (result != NULL) {
        memset(result, 0, size);
    }
    return result;
}

char *minidb_arena_copy_string(minidb_arena_t *arena, const char *text, size_t length) {
    char *copy = minidb_arena_alloc(arena, length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

void minidb_arena_reset(minidb_arena_t *arena) {
    /* Keep only the first block: steady-state statements then never malloc. */
    if (arena->first != NULL) {
        minidb_arena_block_t *extra = arena->first->next;
        while (extra != NULL) {
            minidb_arena_block_t *next = extra->next;
            free(extra);
            extra = next;
        }
        arena->first->next = NULL;
        arena->first->used = 0;
    }
    arena->current = NULL;
}

void minidb_arena_destroy(minidb_arena_t *arena) {
    minidb_arena_block_t *block = arena->first;
    while (block != NULL) {
        minidb_arena_block_t *next = block->next;
        free(block);
        block = next;
    }
    arena->first = NULL;
    arena->current = NULL;
}
