#include "mc_protocol.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

static int r_byte_array(mc_reader_t *r, const uint8_t **out, size_t *length) {
    int32_t len = 0;
    if (r_varint(r, &len) != 0 || len <= 0 || len > 512 || (size_t)len > r->len - r->pos) return -1;
    *out = r->data + r->pos;
    *length = (size_t)len;
    r->pos += (size_t)len;
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

static int send_encryption_request(mc_conn_t *c) {
    mc_auth_service_t *auth = net_server_auth(c->server);
    size_t key_len = 0;
    const uint8_t *key = mc_auth_public_key(auth, &key_len);
    if (!key || key_len == 0 || key_len > 4096 || mc_auth_random(c->verify_token, sizeof(c->verify_token)) != 0) return -1;
    uint8_t buf[4352];
    size_t pos = 0;
    if (w_string(buf, sizeof(buf), &pos, "") != 0 ||
        w_varint(buf, sizeof(buf), &pos, (int32_t)key_len) != 0 ||
        pos + key_len > sizeof(buf)) return -1;
    memcpy(buf + pos, key, key_len);
    pos += key_len;
    if (w_varint(buf, sizeof(buf), &pos, (int32_t)sizeof(c->verify_token)) != 0 ||
        pos + sizeof(c->verify_token) + 1 > sizeof(buf)) return -1;
    memcpy(buf + pos, c->verify_token, sizeof(c->verify_token));
    pos += sizeof(c->verify_token);
    buf[pos++] = 1; /* request Mojang authentication */
    if (conn_write_packet(c, MC_PKT_LOGIN_CLIENTBOUND_HELLO, buf, pos, -1) != 0) return -1;
    c->verify_token_sent = true;
    return 0;
}

static int handle_encryption_response(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c->cfg || !c->cfg->online_mode || !c->verify_token_sent || c->auth_pending || c->login_success_sent) return -1;
    mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
    const uint8_t *secret_encrypted = NULL, *token_encrypted = NULL;
    size_t secret_encrypted_len = 0, token_encrypted_len = 0;
    if (r_byte_array(&r, &secret_encrypted, &secret_encrypted_len) != 0 ||
        r_byte_array(&r, &token_encrypted, &token_encrypted_len) != 0 || r.pos != r.len) return -1;
    mc_auth_service_t *auth = net_server_auth(c->server);
    uint8_t secret[512], token[512];
    size_t secret_len = 0, token_len = 0;
    if (mc_auth_decrypt(auth, secret_encrypted, secret_encrypted_len, secret, sizeof(secret), &secret_len) != 0 ||
        mc_auth_decrypt(auth, token_encrypted, token_encrypted_len, token, sizeof(token), &token_len) != 0 ||
        secret_len != 16 || token_len != sizeof(c->verify_token)) return -1;
    unsigned mismatch = 0;
    for (size_t i = 0; i < sizeof(c->verify_token); i++) mismatch |= (unsigned)(token[i] ^ c->verify_token[i]);
    if (mismatch) return -1;
    char hash[42];
    size_t public_key_len = 0;
    const uint8_t *public_key = mc_auth_public_key(auth, &public_key_len);
    if (!public_key || mc_auth_server_hash(secret, public_key, public_key_len, hash) != 0 ||
        mc_crypto_enable(c, secret) != 0) return -1;
    c->auth_ticket = mc_auth_submit(auth, c->username, hash);
    if (!c->auth_ticket) return -1;
    c->auth_pending = true;
    c->auth_deadline_ms = mc_now_us() / 1000 + 15000;
    log_info("online crypto established: name=%s; checking Mojang session", c->username);
    memset(secret, 0, sizeof(secret));
    memset(token, 0, sizeof(token));
    memset(c->verify_token, 0, sizeof(c->verify_token));
    atomic_store_explicit(&c->crypto_phase, 2, memory_order_release);
    if (c->notify_output) c->notify_output(c);
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
    if (!c || c->login_success_sent) return 0;
    if (c->auth_pending) {
        if (now_ms >= c->auth_deadline_ms) {
            log_info("online session timeout: name=%s", c->username);
            return -1;
        }
        mc_public_profile_t *profile = malloc(sizeof(*profile));
        if (!profile) return -1;
        int rc = mc_auth_poll(net_server_auth(c->server), c->auth_ticket, profile);
        if (rc == 0) { free(profile); return 0; }
        if (rc != 1) {
            log_info("online session rejected: name=%s", c->username);
            free(profile);
            return -1;
        }
        if (net_server_identity_in_use(c->server, c, profile->name, profile->public_uuid)) {
            log_info("online duplicate identity: name=%s", c->username);
            free(profile);
            return -1;
        }
        memcpy(c->uuid, profile->public_uuid, sizeof(c->uuid));
        c->has_uuid = true;
        snprintf(c->username, sizeof(c->username), "%s", profile->name);
        c->command_authorized = c->cfg &&
            (c->cfg->admin_commands ||
             (c->cfg->has_admin_uuid &&
              memcmp(c->uuid, c->cfg->admin_uuid, sizeof(c->uuid)) == 0));
        apply_public_skin(c, profile);
        free(profile);
        c->auth_pending = false;
        return send_login_success(c);
    }
    if (!c->skin_lookup_pending) return 0;
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
        if (c->username[0] || c->login_success_sent) return -1;
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
        const uint8_t zero_uuid[16] = {0};
        const uint8_t *identity_check = (c->cfg && c->cfg->online_mode) ? zero_uuid : offline_uuid;
        if (net_server_identity_in_use(c->server, c, username, identity_check)) return -1;
        memcpy(c->username, username, sizeof(username));
        if (c->cfg && c->cfg->online_mode) {
            log_info("LoginStart online: name=%s", c->username);
            return send_encryption_request(c);
        }
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

    if (frame->packet_id == MC_PKT_LOGIN_SERVERBOUND_KEY) return handle_encryption_response(c, frame);

    if (frame->packet_id == MC_PKT_LOGIN_SERVERBOUND_LOGIN_ACKNOWLEDGED) {
        if (!c->login_success_sent || frame->payload.len != 0) return -1;
        c->state = MC_STATE_CONFIGURATION;
        return proto_config_send_known_packs(c);
    }

    return -1;
}
