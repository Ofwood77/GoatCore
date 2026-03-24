#include "mc_net.h"
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

int mc_crypto_read(mc_conn_t *c, uint8_t *data, size_t len) {
    if (!c->encryption_enabled) return 0;
    int out_len1 = 0;
    int out_len2 = 0;
    uint8_t *tmp = (uint8_t *)malloc(len + 16);
    if (!tmp) return -1;
    if (EVP_DecryptUpdate((EVP_CIPHER_CTX *)c->ssl_read, tmp, &out_len1, data, (int)len) != 1) {
        free(tmp);
        return -1;
    }
    if (EVP_DecryptFinal_ex((EVP_CIPHER_CTX *)c->ssl_read, tmp + out_len1, &out_len2) != 1) {
        free(tmp);
        return -1;
    }
    memcpy(data, tmp, (size_t)(out_len1 + out_len2));
    free(tmp);
    return 0;
}

int mc_crypto_write(mc_conn_t *c, const uint8_t *data, size_t len, mc_buf_t *out) {
    if (!c->encryption_enabled) return buf_write(out, data, len);
    int out_len1 = 0;
    int out_len2 = 0;
    uint8_t *tmp = (uint8_t *)malloc(len + 16);
    if (!tmp) return -1;
    if (EVP_EncryptUpdate((EVP_CIPHER_CTX *)c->ssl_write, tmp, &out_len1, data, (int)len) != 1) {
        free(tmp);
        return -1;
    }
    if (EVP_EncryptFinal_ex((EVP_CIPHER_CTX *)c->ssl_write, tmp + out_len1, &out_len2) != 1) {
        free(tmp);
        return -1;
    }
    int rc = buf_write(out, tmp, (size_t)(out_len1 + out_len2));
    free(tmp);
    return rc;
}
