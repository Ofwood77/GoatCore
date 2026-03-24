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
int32_t proto_play_item_to_state(const mc_world_ids_t *ids, int32_t item_id);
int32_t proto_play_slot_to_state(const mc_world_ids_t *ids, const mc_slot_t *slot);
int proto_play_encode_chunkdata_for_test(mc_world_t *world, const mc_chunk_t *chunk, mc_buf_t *out);

#endif /* MC_PROTOCOL_H */
