#include "mc_protocol.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mc_reader_t;

static int r_varint(mc_reader_t *r, int32_t *out) {
    size_t n = 0;
    if (r->pos >= r->len) return -1;
    if (varint_read(r->data + r->pos, r->len - r->pos, out, &n) != 0) return -1;
    r->pos += n;
    return 0;
}

static int r_username(mc_reader_t *r, char *out, size_t out_cap) {
    int32_t str_len = 0;
    if (r_varint(r, &str_len) != 0) return -1;
    if (str_len < 3 || str_len > 16 || (size_t)str_len >= out_cap ||
        (size_t)str_len > r->len - r->pos) return -1;
    for (int32_t i = 0; i < str_len; i++) {
        uint8_t ch = r->data[r->pos + (size_t)i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) return -1;
    }
    memcpy(out, r->data + r->pos, (size_t)str_len);
    out[str_len] = '\0';
    r->pos += (size_t)str_len;
    return 0;
}

static int r_uuid(mc_reader_t *r, uint8_t out[16]) {
    if (r->pos + 16 > r->len) return -1;
    memcpy(out, r->data + r->pos, 16);
    r->pos += 16;
    return 0;
}

static int w_varint(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    size_t n = 0;
    if (varint_write(buf + *pos, cap - *pos, v, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int w_string(uint8_t *buf, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    if (w_varint(buf, cap, pos, (int32_t)len) != 0) return -1;
    if (*pos + len > cap) return -1;
    memcpy(buf + *pos, s, len);
    *pos += len;
    return 0;
}

static int w_uuid(uint8_t *buf, size_t cap, size_t *pos, const uint8_t uuid[16]) {
    if (*pos + 16 > cap) return -1;
    memcpy(buf + *pos, uuid, 16);
    *pos += 16;
    return 0;
}

static int send_login_success(mc_conn_t *c) {
    uint8_t buf[512];
    size_t pos = 0;

    if (!c->has_uuid) return -1;
    if (w_uuid(buf, sizeof(buf), &pos, c->uuid) != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, c->username) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1; /* properties count */

    if (conn_write_packet(c, MC_PKT_LOGIN_CLIENTBOUND_LOGIN_FINISHED, buf, pos, -1) != 0) return -1;
    c->login_success_sent = true;
    return 0;
}

int proto_handle_login(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame) return -1;

    if (frame->packet_id == MC_PKT_LOGIN_SERVERBOUND_HELLO) {
        if (c->username[0] || c->login_success_sent || (c->cfg && c->cfg->online_mode)) return -1;
        mc_reader_t r;
        r.data = frame->payload.data;
        r.len = frame->payload.len;
        r.pos = 0;

        char username[sizeof(c->username)] = {0};
        uint8_t ignored_client_uuid[16];
        uint8_t offline_uuid[16];
        if (r_username(&r, username, sizeof(username)) != 0 ||
            r_uuid(&r, ignored_client_uuid) != 0 || r.pos != r.len) return -1;
        /* The client-supplied UUID is never an offline identity claim. */
        proto_fill_offline_uuid(username, offline_uuid);
        if (net_server_identity_in_use(c->server, c, username, offline_uuid)) return -1;
        memcpy(c->username, username, sizeof(username));
        memcpy(c->uuid, offline_uuid, sizeof(c->uuid));
        c->has_uuid = true;
        c->command_authorized = c->cfg &&
            (c->cfg->admin_commands ||
             (c->cfg->admin_username && *c->cfg->admin_username &&
              strcmp(c->username, c->cfg->admin_username) == 0));
        log_info("LoginStart: name=%s", c->username);
        return send_login_success(c);
    }

    if (frame->packet_id == MC_PKT_LOGIN_SERVERBOUND_LOGIN_ACKNOWLEDGED) {
        if (!c->login_success_sent || frame->payload.len != 0) return -1;
        c->state = MC_STATE_CONFIGURATION;
        return proto_config_send_known_packs(c);
    }

    return -1;
}
