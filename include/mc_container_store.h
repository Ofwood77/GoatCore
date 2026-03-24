#ifndef MC_CONTAINER_STORE_H
#define MC_CONTAINER_STORE_H

#include "mc_inventory.h"

int mc_container_store_load(const char *world_path, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                            mc_container_instance_t *out);
int mc_container_store_save(const char *world_path, const mc_container_instance_t *container);
int mc_container_store_delete(const char *world_path, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z);

#endif
