#ifndef MC_PLAYER_SAVE_H
#define MC_PLAYER_SAVE_H
#include <stdbool.h>
#include <stdint.h>

/* Only hotbar selection may wait. Inventory saves keep their existing barriers.
 * Tick-owned, connection-local; no background task holds this or a player. */
#define MC_PLAYER_HOTBAR_FLUSH_MS INT64_C(30000)
typedef struct {
    uint64_t current, durable, in_flight;
    int64_t due_ms;
} mc_player_save_state_t;
int mc_player_save_mark(mc_player_save_state_t *state, int64_t now_ms);
bool mc_player_save_due(const mc_player_save_state_t *state, int64_t now_ms);
uint64_t mc_player_save_begin(mc_player_save_state_t *state);
int mc_player_save_complete(mc_player_save_state_t *state, uint64_t generation, bool success);
#endif
