#ifndef MC_BLOCK_DROPS_H
#define MC_BLOCK_DROPS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t item_id;
    int32_t count;
} mc_block_drop_t;

bool mc_block_drop_resolve_default(int32_t state_id, bool allow_default_drop, int32_t x, int32_t y, int32_t z,
                                   int64_t now_ms, mc_block_drop_t *out_drop);

#endif /* MC_BLOCK_DROPS_H */
