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
    size_t cap = 128 + proto_profile_properties_capacity(c);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;
    size_t pos = 0;

    if (!c->has_uuid || w_uuid(buf, cap, &pos, c->uuid) != 0 ||
        w_string(buf, cap, &pos, c->username) != 0 ||
        proto_profile_write_properties(c, buf, cap, &pos) != 0) {
        free(buf);
        return -1;
    }

    int rc = conn_write_packet(c, MC_PKT_LOGIN_CLIENTBOUND_LOGIN_FINISHED, buf, pos, -1);
    free(buf);
    if (rc != 0) return -1;
    c->login_success_sent = true;
    return 0;
}

static void apply_public_skin(mc_conn_t *c, const mc_public_profile_t *profile) {
    if (!c || !profile || !profile->found) return;
    char *value = strdup(profile->texture_value);
    char *signature = strdup(profile->texture_signature);
    if (!value || !signature) { free(value); free(signature); return; }
    free(c->skin_value);
    free(c->skin_signature);
    c->skin_value = value;
    c->skin_signature = signature;
}

int proto_login_tick(mc_conn_t *c, int64_t now_ms) {
    if (!c || !c->skin_lookup_pending || c->login_success_sent) return 0;
    if (now_ms < c->skin_lookup_deadline_ms) {
        mc_public_profile_t *profile = malloc(sizeof(*profile));
        if (profile) {
            int rc = mc_profile_service_lookup(net_server_profiles(c->server), c->username, profile);
            if (rc == 0) { free(profile); return 0; }
            if (rc == 1) apply_public_skin(c, profile);
            free(profile);
        }
    }
    c->skin_lookup_pending = false;
    return send_login_success(c);
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
        if (c->cfg && c->cfg->offline_skins) {
            mc_public_profile_t *profile = malloc(sizeof(*profile));
            if (profile) {
                int rc = mc_profile_service_lookup(net_server_profiles(c->server), c->username, profile);
                if (rc == 0) {
                    c->skin_lookup_pending = true;
                    c->skin_lookup_deadline_ms = mc_now_us() / 1000 + 4000;
                    free(profile);
                    return 0;
                }
                if (rc == 1) apply_public_skin(c, profile);
                free(profile);
            }
        }
        return send_login_success(c);
    }

    if (frame->packet_id == MC_PKT_LOGIN_SERVERBOUND_LOGIN_ACKNOWLEDGED) {
        if (!c->login_success_sent || frame->payload.len != 0) return -1;
        c->state = MC_STATE_CONFIGURATION;
        return proto_config_send_known_packs(c);
    }

    return -1;
}
