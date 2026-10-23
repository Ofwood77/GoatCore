#include "mc_auth.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fake_fetch(const char *name, const char *hash, mc_public_profile_t *out, void *userdata) {
    (void)userdata;
    if (strcmp(name, "Denied") == 0 || strcmp(hash, "bad") == 0) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->public_uuid, "0123456789abcdef", 16);
    snprintf(out->name, sizeof(out->name), "%s", name);
    return 0;
}

static int wait_auth(mc_auth_service_t *service, uint64_t ticket, mc_public_profile_t *profile) {
    for (int i = 0; i < 300; i++) {
        int rc = mc_auth_poll(service, ticket, profile);
        if (rc != 0) return rc;
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    assert(!"authentication worker did not finish");
    return -2;
}

int main(void) {
    mc_auth_service_t *service = mc_auth_service_create_with_fetch(fake_fetch, NULL);
    assert(service);
    size_t public_len = 0;
    const uint8_t *public_der = mc_auth_public_key(service, &public_len);
    assert(public_der && public_len > 200);
    const unsigned char *cursor = public_der;
    EVP_PKEY *public_key = d2i_PUBKEY(NULL, &cursor, (long)public_len);
    assert(public_key && cursor == public_der + public_len);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(public_key, NULL);
    assert(ctx && EVP_PKEY_encrypt_init(ctx) == 1);
    const uint8_t secret[16] = "test-secret-123";
    uint8_t encrypted[512], plain[512];
    size_t encrypted_len = sizeof(encrypted), plain_len = 0;
    assert(EVP_PKEY_encrypt(ctx, encrypted, &encrypted_len, secret, sizeof(secret)) == 1);
    assert(mc_auth_decrypt(service, encrypted, encrypted_len, plain, sizeof(plain), &plain_len) == 0);
    assert(plain_len == 16 && memcmp(plain, secret, 16) == 0);
    encrypted[encrypted_len - 1] ^= 0x40;
    int tampered_rc = mc_auth_decrypt(service, encrypted, encrypted_len, plain, sizeof(plain), &plain_len);
    assert(tampered_rc != 0 || plain_len != 16 || memcmp(plain, secret, 16) != 0);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(public_key);

    uint8_t zeros[16] = {0};
    char hash[42];
    assert(mc_auth_server_hash(zeros, (const uint8_t *)"abc", 3, hash) == 0);
    assert(strcmp(hash, "-4fe2788697396892810bd9b773636ddc496ddbe6") == 0);
    uint8_t one = 1;
    assert(mc_auth_server_hash(zeros, &one, 1, hash) == 0);
    assert(strcmp(hash, "1b69d765862555d95cdd99e762a688954ad65ec3") == 0);

    mc_public_profile_t profile = {0};
    uint64_t ticket = mc_auth_submit(service, "ValidUser", hash);
    assert(ticket && wait_auth(service, ticket, &profile) == 1);
    assert(strcmp(profile.name, "ValidUser") == 0);
    assert(mc_auth_poll(service, ticket, &profile) == -2);
    ticket = mc_auth_submit(service, "Denied", hash);
    assert(ticket && wait_auth(service, ticket, &profile) == -1);
    assert(mc_auth_submit(service, "Bad-Name", hash) == 0);
    assert(mc_auth_submit(service, "ValidUser", "oops&inject=1") == 0);
    mc_auth_service_destroy(service);
    puts("auth: RSA, signed Java hash, async allow/deny and bounded input ok");
    return 0;
}
