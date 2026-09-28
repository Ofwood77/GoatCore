#ifndef MC_PROTOCOL_H
#define MC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mc_net.h"

#define MC_VARINT_MAX_BYTES 5

int varint_read(const uint8_t *buf, size_t buf_len, int32_t *out, size_t *bytes_read);
int varint_write(uint8_t *buf, size_t buf_len, int32_t val, size_t *bytes_written);

int proto_handle_handshake(mc_conn_t *c, const mc_frame_t *frame);
int proto_handle_status(mc_conn_t *c, const mc_frame_t *frame, const char *motd_json, int32_t online_players, int32_t max_players);
int proto_handle_login(mc_conn_t *c, const mc_frame_t *frame);
int proto_config_handle(mc_conn_t *c, const mc_frame_t *frame);
int proto_config_send_known_packs(mc_conn_t *c);
int proto_config_send_registry(mc_conn_t *c);
int proto_play_send_initial(mc_conn_t *c);
int proto_play_handle(mc_conn_t *c, const mc_frame_t *frame, int64_t now_ms);
int proto_play_tick(mc_conn_t *c, int64_t now_ms);
int proto_send_play_disconnect(mc_conn_t *c, const char *reason_json);
void proto_play_conn_cleanup(mc_conn_t *c);
void proto_fill_offline_uuid(const char *username, uint8_t out[16]);
int proto_play_sync_remote_player(mc_conn_t *viewer, mc_conn_t *subject);
int proto_play_remove_remote_player(mc_conn_t *viewer, mc_conn_t *subject);
int proto_play_send_difficulty(mc_conn_t *c);
int proto_play_send_system_message(mc_conn_t *c, const char *text);
int32_t proto_play_item_to_state(const mc_world_ids_t *ids, int32_t item_id);
int32_t proto_play_slot_to_state(const mc_world_ids_t *ids, const mc_slot_t *slot);
int32_t proto_play_resolve_placement_state(const mc_world_ids_t *ids, const mc_slot_t *slot, int32_t face, float yaw, float pitch);
int32_t proto_play_resolve_placement_state_ex(const mc_world_ids_t *ids, const mc_slot_t *slot, int32_t face, float yaw, float pitch,
                                             float hit_x, float hit_y, float hit_z);
int32_t proto_play_try_merge_slab_state_for_test(int32_t clicked_state_id, int32_t requested_state_id);
int32_t proto_play_recompute_connected_block_state_for_test(mc_world_t *world, int32_t x, int32_t y, int32_t z, int32_t state_id);
int32_t proto_play_serverbound_packet_id_for_test(const char *name);
int proto_play_parse_gamemode_for_test(const char *mode);
int proto_play_parse_interact_attack_for_test(const uint8_t *data, size_t len, int32_t *out_target, bool *out_is_attack);
float proto_play_pvp_damage_for_test(const mc_conn_t *attacker, const mc_conn_t *target, float base_damage);
int proto_play_apply_pvp_attack_for_test(mc_conn_t *attacker, mc_conn_t *target, bool pvp_enabled, int64_t now_ms);
bool proto_play_can_pickup_items_for_test(const mc_conn_t *c);
int proto_play_inventory_stack_count_for_test(const mc_conn_t *c);
int proto_play_apply_damage_for_test(mc_conn_t *c, float amount, bool server_locked, int64_t now_ms);
void proto_play_respawn_state_reset_for_test(mc_conn_t *c);
int proto_play_drop_selected_for_test(mc_conn_t *c, int32_t requested_count);
int proto_play_set_creative_slot_for_test(mc_conn_t *c, int16_t slot_id, const mc_slot_t *slot);
int proto_play_try_pickup_ground_slot(mc_conn_t *c, mc_slot_t *ground_slot);
int proto_play_encode_chunkdata_for_test(mc_world_t *world, const mc_chunk_t *chunk, mc_buf_t *out);
int proto_play_validate_chunkdata_for_test(const uint8_t *data, size_t len);

#endif /* MC_PROTOCOL_H */
