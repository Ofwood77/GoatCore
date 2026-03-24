#include "mc_net.h"

int mc_crypto_read(mc_conn_t *c, uint8_t *data, size_t len) {
    (void)c;
    (void)data;
    (void)len;
    return 0;
}

int mc_crypto_write(mc_conn_t *c, const uint8_t *data, size_t len, mc_buf_t *out) {
    (void)c;
    return buf_write(out, data, len);
}
