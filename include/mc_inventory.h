#ifndef MC_INVENTORY_H
#define MC_INVENTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_PLAYER_SLOT_COUNT 46
#define MC_PLAYER_HOTBAR_BASE 36
#define MC_PLAYER_HOTBAR_SIZE 9
#define MC_CONTAINER_SLOT_COUNT 27

typedef enum {
    MC_CONTAINER_KIND_NONE = 0,
    MC_CONTAINER_KIND_CHEST = 1,
    MC_CONTAINER_KIND_ENDER_CHEST = 2,
} mc_container_kind_t;

typedef struct {
    bool present;
    int32_t item_id;
    int32_t count;
    int32_t damage;
    int32_t added_component_count;
    int32_t removed_component_count;
    uint8_t *components;
    size_t components_len;
} mc_slot_t;

typedef struct {
    mc_slot_t slots[MC_PLAYER_SLOT_COUNT];
    mc_slot_t cursor_slot;
    int32_t selected_hotbar_slot;
    int32_t state_id;
} mc_inventory_t;

typedef struct {
    int32_t window_id;
    int32_t window_type;
    int32_t slot_count;
    int32_t state_id;
    mc_slot_t *slots;
} mc_window_t;

typedef mc_window_t mc_container_t;

typedef struct {
    mc_container_kind_t kind;
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t slot_count;
    int32_t state_id;
    bool dirty;
    mc_slot_t slots[MC_CONTAINER_SLOT_COUNT];
} mc_container_instance_t;

typedef struct {
    bool open;
    int32_t window_id;
    int32_t window_type;
    int32_t slot_count;
    mc_container_instance_t *container;
} mc_active_window_t;

typedef struct mc_player_data {
    uint8_t uuid[16];
    bool has_uuid;
    char username[17];
    int32_t gamemode;
    double pos_x;
    double pos_y;
    double pos_z;
    float yaw;
    float pitch;
    mc_inventory_t inventory;
    mc_slot_t ender_chest[MC_CONTAINER_SLOT_COUNT];
    int32_t ender_state_id;
} mc_player_data_t;

void mc_slot_init(mc_slot_t *slot);
void mc_slot_clear(mc_slot_t *slot);
void mc_slot_move(mc_slot_t *dst, mc_slot_t *src);
int mc_slot_copy(mc_slot_t *dst, const mc_slot_t *src);
bool mc_slot_is_same_item(const mc_slot_t *a, const mc_slot_t *b);
int mc_slot_set_simple(mc_slot_t *slot, int32_t item_id, int32_t count);

void mc_inventory_init(mc_inventory_t *inv);
void mc_inventory_clear(mc_inventory_t *inv);
void mc_inventory_fill_starter_loadout(mc_inventory_t *inv);
int mc_inventory_selected_slot_index(const mc_inventory_t *inv);
mc_slot_t *mc_inventory_selected_slot(mc_inventory_t *inv);
const mc_slot_t *mc_inventory_selected_slot_const(const mc_inventory_t *inv);

void mc_container_instance_init(mc_container_instance_t *container, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z);
void mc_container_instance_clear(mc_container_instance_t *container);

void mc_player_data_init(mc_player_data_t *player);
void mc_player_data_clear(mc_player_data_t *player);

int mc_slot_read_net(const uint8_t *buf, size_t len, size_t *pos, mc_slot_t *out);
int mc_slot_write_net(uint8_t *buf, size_t cap, size_t *pos, const mc_slot_t *slot);

#endif
