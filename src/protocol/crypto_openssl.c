#include "mc_net.h"
#include <openssl/evp.h>
#include <limits.h>
#include <stdlib.h>

int mc_crypto_enable(mc_conn_t *c, const uint8_t secret[16]) {
    if (!c || !secret || c->ssl_read || c->ssl_write) return -1;
    EVP_CIPHER_CTX *read_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX *write_ctx = EVP_CIPHER_CTX_new();
    if (!read_ctx || !write_ctx ||
        EVP_DecryptInit_ex(read_ctx, EVP_aes_128_cfb8(), NULL, secret, secret) != 1 ||
        EVP_EncryptInit_ex(write_ctx, EVP_aes_128_cfb8(), NULL, secret, secret) != 1) {
        EVP_CIPHER_CTX_free(read_ctx);
        EVP_CIPHER_CTX_free(write_ctx);
        return -1;
    }
    c->ssl_read = read_ctx;
    c->ssl_write = write_ctx;
    return 0;
}

void mc_crypto_free(mc_conn_t *c) {
    if (!c) return;
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)c->ssl_read);
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)c->ssl_write);
    c->ssl_read = NULL;
    c->ssl_write = NULL;
}

int mc_crypto_read(mc_conn_t *c, uint8_t *data, size_t len) {
    if (!c || (!data && len)) return -1;
    if (atomic_load_explicit(&c->crypto_phase, memory_order_acquire) != 2 || len == 0) return 0;
    if (!c->ssl_read || len > INT_MAX) return -1;
    int output_len = 0;
    return EVP_DecryptUpdate((EVP_CIPHER_CTX *)c->ssl_read, data, &output_len,
                             data, (int)len) == 1 && output_len == (int)len ? 0 : -1;
}

int mc_crypto_write(mc_conn_t *c, const uint8_t *data, size_t len, mc_buf_t *out) {
    if (!c || !out || (!data && len)) return -1;
    if (atomic_load_explicit(&c->crypto_phase, memory_order_acquire) != 2) return buf_write(out, data, len);
    if (!c->ssl_write || len > INT_MAX) return -1;
    uint8_t *encrypted = malloc(len ? len : 1);
    if (!encrypted) return -1;
    int output_len = 0;
    int rc = -1;
    if (EVP_EncryptUpdate((EVP_CIPHER_CTX *)c->ssl_write, encrypted, &output_len,
                          data, (int)len) == 1 && output_len == (int)len)
        rc = buf_write(out, encrypted, len);
    free(encrypted);
    return rc;
}
