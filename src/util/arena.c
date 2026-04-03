#include "arena.h"

#include <stdlib.h>
#include <string.h>

static size_t arena_align_up(size_t value, size_t align) {
    size_t mask = align - 1;
    return (value + mask) & ~mask;
}

int mc_arena_init(mc_arena_t *arena, size_t capacity) {
    if (!arena || capacity == 0) return -1;

    arena->base = (uint8_t *)malloc(capacity);
    if (!arena->base) return -1;

    arena->capacity = capacity;
    arena->offset = 0;
    return 0;
}

void mc_arena_destroy(mc_arena_t *arena) {
    if (!arena) return;
    free(arena->base);
    arena->base = NULL;
    arena->capacity = 0;
    arena->offset = 0;
}

void *mc_arena_alloc(mc_arena_t *arena, size_t size) {
    size_t offset;
    size_t next;

    if (!arena) return NULL;
    if (size == 0) size = 1;

    offset = arena_align_up(arena->offset, sizeof(void *));
    if (offset > arena->capacity) return NULL;
    if (size > arena->capacity - offset) return NULL;

    next = offset + size;
    arena->offset = next;
    return arena->base + offset;
}

void mc_arena_reset(mc_arena_t *arena) {
    if (!arena) return;
    arena->offset = 0;
    if (arena->base && arena->capacity > 0) {
        memset(arena->base, 0, arena->capacity);
    }
}
