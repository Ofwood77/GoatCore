#ifndef MC_ARENA_H
#define MC_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t offset;
} mc_arena_t;

int mc_arena_init(mc_arena_t *arena, size_t capacity);
void mc_arena_destroy(mc_arena_t *arena);
void *mc_arena_alloc(mc_arena_t *arena, size_t size);
void mc_arena_reset(mc_arena_t *arena);

#endif /* MC_ARENA_H */
