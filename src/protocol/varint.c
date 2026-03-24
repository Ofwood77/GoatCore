#include "mc_protocol.h"

int varint_read(const uint8_t *buf, size_t buf_len, int32_t *out, size_t *bytes_read) {
    int32_t result = 0;
    int shift = 0;
    size_t i = 0;
    while (i < buf_len && i < MC_VARINT_MAX_BYTES) {
        uint8_t byte = buf[i++];
        result |= (int32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = result;
            *bytes_read = i;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int varint_write(uint8_t *buf, size_t buf_len, int32_t val, size_t *bytes_written) {
    size_t i = 0;
    uint32_t u = (uint32_t)val;
    do {
        if (i >= buf_len) return -1;
        uint8_t temp = u & 0x7F;
        u >>= 7;
        if (u != 0) temp |= 0x80;
        buf[i++] = temp;
    } while (u != 0);
    *bytes_written = i;
    return 0;
}
