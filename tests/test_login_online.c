#include "mc_protocol.h"
#include "generated_minecraft_ids.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static mc_auth_service_t *auth;
static int sent_packet_id;
static uint8_t sent_payload[32768];
static size_t sent_len;

mc_auth_service_t *net_server_auth(mc_server_t *server) { (void)server; return auth; }
mc_profile_service_t *net_server_profiles(mc_server_t *server) { (void)server; return NULL; }
int mc_profile_service_lookup(mc_profile_service_t *service, const char *name, mc_public_profile_t *out) {
    (void)service; (void)name; (void)out; return -1;
}
bool net_server_identity_in_use(mc_server_t *server, const mc_conn_t *exclude,
                                const char *name, const uint8_t uuid[16]) {
    (void)server; (void)exclude; (void)name; (void)uuid; return false;
}
int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload,
                      size_t len, int compression_threshold) {
    (void)c; (void)compression_threshold;
    assert(len <= sizeof(sent_payload));
    sent_packet_id = packet_id;
    sent_len = len;
    memcpy(sent_payload, payload, len);
    return 0;
}
int proto_config_send_known_packs(mc_conn_t *c) { (void)c; return 0; }

static int fake_fetch(const char *name, const char *hash, mc_public_profile_t *out, void *userdata) {
    (void)userdata;
    assert(hash[0]);
    memset(out, 0, sizeof(*out));
    memcpy(out->public_uuid, "0123456789abcdef", 16);
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->found = true;
    snprintf(out->texture_value, sizeof(out->texture_value), "QUJDRA==");
    snprintf(out->texture_signature, sizeof(out->texture_signature), "RUZHSA==");
    return 0;
}

static size_t read_varint(const uint8_t *data, size_t length, size_t *pos) {
    int32_t result = 0;
    size_t used = 0;
    assert(*pos < length && varint_read(data + *pos, length - *pos, &result, &used) == 0);
    assert(result >= 0);
    *pos += used;
    return (size_t)result;
}

static size_t put_varint(uint8_t *data, size_t cap, size_t pos, int32_t value) {
    size_t used = 0;
    assert(pos < cap && varint_write(data + pos, cap - pos, value, &used) == 0);
    return pos + used;
}

static size_t encrypt_with(EVP_PKEY *key, const uint8_t *plain, size_t plain_len,
                           uint8_t *out, size_t cap) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
    assert(ctx && EVP_PKEY_encrypt_init(ctx) == 1);
    assert(EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1);
    size_t len = cap;
    assert(EVP_PKEY_encrypt(ctx, out, &len, plain, plain_len) == 1);
    EVP_PKEY_CTX_free(ctx);
    return len;
}

static void run_login(bool tamper_token, bool admin_uuid_matches) {
    mc_server_config_t cfg = {.online_mode = true, .has_admin_uuid = true,
                              .admin_username = "ValidUser"};
    memcpy(cfg.admin_uuid, admin_uuid_matches ? "0123456789abcdef" :
                                         "not-the-admin!!", 16);
    mc_conn_t conn = {0};
    conn.cfg = &cfg;
    conn.server = (mc_server_t *)1;
    conn.state = MC_STATE_LOGIN;
    atomic_init(&conn.crypto_phase, 0);
    sent_packet_id = -1;
    uint8_t hello[1 + 9 + 16] = {9, 'V','a','l','i','d','U','s','e','r'};
    memset(hello + 10, 0xaa, 16); /* forged UUID must be ignored */
    mc_frame_t frame = {.packet_id = MC_PKT_LOGIN_SERVERBOUND_HELLO,
                        .payload = {.data = hello, .len = sizeof(hello)}};
    assert(proto_handle_login(&conn, &frame) == 0);
    assert(sent_packet_id == MC_PKT_LOGIN_CLIENTBOUND_HELLO && !conn.has_uuid);
    size_t pos = 0;
    assert(read_varint(sent_payload, sent_len, &pos) == 0);
    size_t key_len = read_varint(sent_payload, sent_len, &pos);
    const unsigned char *der = sent_payload + pos;
    EVP_PKEY *key = d2i_PUBKEY(NULL, &der, (long)key_len);
    assert(key && der == sent_payload + pos + key_len);
    pos += key_len;
    size_t token_len = read_varint(sent_payload, sent_len, &pos);
    assert(token_len == 16 && sent_payload[pos + token_len] == 1);
    uint8_t token[16];
    memcpy(token, sent_payload + pos, 16);
    if (tamper_token) token[0] ^= 1;
    uint8_t secret[16] = "server-secret!!";
    uint8_t encrypted_secret[512], encrypted_token[512];
    size_t secret_len = encrypt_with(key, secret, 16, encrypted_secret, sizeof(encrypted_secret));
    size_t encrypted_token_len = encrypt_with(key, token, 16, encrypted_token, sizeof(encrypted_token));
    EVP_PKEY_free(key);
    uint8_t response[1030];
    pos = put_varint(response, sizeof(response), 0, (int32_t)secret_len);
    memcpy(response + pos, encrypted_secret, secret_len);
    pos += secret_len;
    pos = put_varint(response, sizeof(response), pos, (int32_t)encrypted_token_len);
    memcpy(response + pos, encrypted_token, encrypted_token_len);
    pos += encrypted_token_len;
    frame.packet_id = MC_PKT_LOGIN_SERVERBOUND_KEY;
    frame.payload.data = response;
    frame.payload.len = pos;
    if (tamper_token) {
        assert(proto_handle_login(&conn, &frame) == -1);
        assert(!conn.has_uuid && !conn.auth_pending);
    } else {
        assert(proto_handle_login(&conn, &frame) == 0);
        assert(atomic_load(&conn.crypto_phase) == 2 && conn.auth_pending && !conn.has_uuid);
        for (int i = 0; i < 300 && !conn.login_success_sent; i++) {
            assert(proto_login_tick(&conn, 0) == 0);
            if (!conn.login_success_sent) {
                struct timespec delay = {0, 10000000};
                nanosleep(&delay, NULL);
            }
        }
        assert(conn.login_success_sent && conn.has_uuid);
        assert(conn.command_authorized == admin_uuid_matches);
        assert(memcmp(conn.uuid, "0123456789abcdef", 16) == 0);
        assert(sent_packet_id == MC_PKT_LOGIN_CLIENTBOUND_LOGIN_FINISHED);
        assert(memcmp(sent_payload, conn.uuid, 16) == 0);
        pos = 16;
        size_t name_len = read_varint(sent_payload, sent_len, &pos);
        assert(name_len == 9 && memcmp(sent_payload + pos, "ValidUser", 9) == 0);
        pos += name_len;
        assert(read_varint(sent_payload, sent_len, &pos) == 1);
        assert(conn.skin_value && conn.skin_signature);
    }
    free(conn.skin_value);
    free(conn.skin_signature);
    mc_crypto_free(&conn);
}

int main(void) {
    auth = mc_auth_service_create_with_fetch(fake_fetch, NULL);
    assert(auth);
    run_login(false, true);
    run_login(false, false); /* matching name alone grants no online privileges */
    run_login(true, true);
    mc_auth_service_destroy(auth);
    puts("online login: official UUID and signed skin only after RSA/token/session verification");
    return 0;
}
