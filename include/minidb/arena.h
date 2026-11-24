#ifndef MINIDB_ARENA_H
#define MINIDB_ARENA_H

#include <stddef.h>

/*
 * Bump allocator for per-statement memory.
 *
 * Everything a statement needs for its lifetime (AST nodes, operators, row
 * buffers) is carved out of one arena and released together by
 * minidb_arena_reset(). Individual allocations are never freed.
 *
 * Ownership: the arena owns every block it hands out. Callers own the arena
 * itself and must call minidb_arena_destroy() exactly once.
 */

typedef struct minidb_arena_block minidb_arena_block_t;

typedef struct {
    minidb_arena_block_t *current;  /* block allocations are carved from */
    minidb_arena_block_t *first;    /* kept across resets to avoid malloc churn */
} minidb_arena_t;

void minidb_arena_init(minidb_arena_t *arena);

/* Returns `size` bytes aligned for any type, or NULL when out of memory. */
void *minidb_arena_alloc(minidb_arena_t *arena, size_t size);

/* Same as minidb_arena_alloc, with the bytes zeroed. */
void *minidb_arena_alloc_zeroed(minidb_arena_t *arena, size_t size);

/* Copies `length` bytes of `text` and appends a NUL terminator. */
char *minidb_arena_copy_string(minidb_arena_t *arena, const char *text, size_t length);

/* Invalidates every allocation but keeps the first block for reuse. */
void minidb_arena_reset(minidb_arena_t *arena);

/* Releases every block. The arena may be reused after another init. */
void minidb_arena_destroy(minidb_arena_t *arena);

#endif
