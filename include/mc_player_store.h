#ifndef MC_PLAYER_STORE_H
#define MC_PLAYER_STORE_H

#include "mc_inventory.h"

int mc_player_store_load(const char *world_path, const uint8_t uuid[16], bool has_uuid, const char *username, mc_player_data_t *out);
int mc_player_store_save(const char *world_path, const mc_player_data_t *player);
int mc_player_store_save_to_file(const char *world_path, const mc_player_data_t *player);

#endif
