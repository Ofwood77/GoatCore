#include "mc_protocol.h"
#include "generated_minecraft_ids.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool duplicate_identity;
static int sent_packet_id = -1;
static uint8_t sent_payload[512];
static size_t sent_payload_len;

bool net_server_identity_in_use(mc_server_t *server, const mc_conn_t *exclude, const char *name, const uint8_t uuid[16]) {
    (void)server; (void)exclude; (void)name; (void)uuid;
    return duplicate_identity;
}

mc_profile_service_t *net_server_profiles(mc_server_t *server) { (void)server; return NULL; }
mc_auth_service_t *net_server_auth(mc_server_t *server) { (void)server; return NULL; }
int mc_profile_service_lookup(mc_profile_service_t *service, const char *name, mc_public_profile_t *out) {
    (void)service; (void)name; (void)out; return -1;
}

int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload, size_t len, int compression_threshold) {
    (void)c; (void)compression_threshold;
    assert(len <= sizeof(sent_payload));
    sent_packet_id = packet_id;
    sent_payload_len = len;
    if (len) memcpy(sent_payload, payload, len);
    return 0;
}

int proto_config_send_known_packs(mc_conn_t *c) { (void)c; return 0; }

static void init_conn(mc_conn_t *c, mc_server_config_t *cfg) {
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    c->state = MC_STATE_LOGIN;
    sent_packet_id = -1;
    sent_payload_len = 0;
    duplicate_identity = false;
}

static mc_frame_t login_frame(const char *name, uint8_t *buf, size_t cap) {
    size_t name_len = strlen(name);
    assert(name_len + 17 <= cap);
    buf[0] = (uint8_t)name_len;
    memcpy(buf + 1, name, name_len);
    memset(buf + 1 + name_len, 0xab, 16); /* forged client UUID */
    mc_frame_t frame = {0};
    frame.packet_id = MC_PKT_LOGIN_SERVERBOUND_HELLO;
    frame.payload.data = buf;
    frame.payload.len = name_len + 17;
    return frame;
}

int main(void) {
    static const uint8_t notch_uuid[16] = {
        0xb5,0x0a,0xd3,0x85,0x82,0x9d,0x31,0x41,
        0xa2,0x16,0x7e,0x7d,0x75,0x39,0xba,0x7f
    };
    static const uint8_t legacy_notch_uuid[16] = {
        0xea,0x0e,0x97,0x20,0x84,0xa5,0x34,0xef,
        0xaf,0xaf,0xfa,0xa3,0x26,0x89,0xc0,0xdf
    };
    uint8_t uuid[16];
    proto_fill_offline_uuid("Notch", uuid);
    assert(memcmp(uuid, notch_uuid, 16) == 0);
    proto_fill_legacy_offline_uuid("Notch", uuid);
    assert(memcmp(uuid, legacy_notch_uuid, 16) == 0);
    proto_fill_offline_uuid("Notch", uuid);
    assert(memcmp(uuid, notch_uuid, 16) == 0);

    mc_server_config_t cfg = {0};
    mc_conn_t conn;
    uint8_t buf[64];
    init_conn(&conn, &cfg);
    mc_frame_t frame = login_frame("Notch", buf, sizeof(buf));
    assert(proto_handle_login(&conn, &frame) == 0);
    assert(conn.has_uuid && memcmp(conn.uuid, notch_uuid, 16) == 0);
    assert(sent_packet_id == MC_PKT_LOGIN_CLIENTBOUND_LOGIN_FINISHED);
    assert(sent_payload_len > 16 && memcmp(sent_payload, notch_uuid, 16) == 0);
    assert(proto_handle_login(&conn, &frame) == -1); /* repeated hello */

    mc_frame_t ack = {0};
    ack.packet_id = MC_PKT_LOGIN_SERVERBOUND_LOGIN_ACKNOWLEDGED;
    assert(proto_handle_login(&conn, &ack) == 0);
    assert(conn.state == MC_STATE_CONFIGURATION);

    init_conn(&conn, &cfg);
    assert(proto_handle_login(&conn, &ack) == -1); /* early ack */
    frame = login_frame("ab", buf, sizeof(buf));
    assert(proto_handle_login(&conn, &frame) == -1);
    frame = login_frame("Bad-Name", buf, sizeof(buf));
    assert(proto_handle_login(&conn, &frame) == -1);
    frame = login_frame("Notch", buf, sizeof(buf));
    frame.payload.len--; /* truncated UUID */
    assert(proto_handle_login(&conn, &frame) == -1);
    frame.payload.len++;
    buf[frame.payload.len] = 0;
    frame.payload.len++; /* unexpected extra data */
    assert(proto_handle_login(&conn, &frame) == -1);
    frame.payload.len--;
    ack.payload.data = buf;
    ack.payload.len = 1;
    assert(proto_handle_login(&conn, &ack) == -1);
    ack.payload.len = 0;
    duplicate_identity = true;
    assert(proto_handle_login(&conn, &frame) == -1);
    assert(!conn.has_uuid && sent_packet_id == -1);
    duplicate_identity = false;
    cfg.online_mode = true;
    assert(proto_handle_login(&conn, &frame) == -1); /* fail closed */
    puts("login: offline UUID, forged UUID, malformed and duplicate identities ok");
    return 0;
}
