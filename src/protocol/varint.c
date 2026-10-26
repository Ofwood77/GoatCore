#include "mc_protocol.h"
#include <math.h>

int varint_read(const uint8_t *buf, size_t buf_len, int32_t *out, size_t *bytes_read) {
    uint32_t result = 0;
    int shift = 0;
    size_t i = 0;
    while (i < buf_len && i < MC_VARINT_MAX_BYTES) {
        uint8_t byte = buf[i++];
        if (i == 5 && (byte & 0xF0)) return -1;
        result |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = (int32_t)result;
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

static double lpvec3_sanitize(double value) {
    if (isnan(value)) return 0.0;
    return fmax(-17179869183.0, fmin(17179869183.0, value));
}

static uint64_t lpvec3_pack(double value) {
    return (uint64_t)floor(((value * 0.5 + 0.5) * 32766.0) + 0.5);
}

int mc_lpvec3_write(uint8_t *buf, size_t cap, size_t *pos, double x, double y, double z) {
    if (!buf || !pos || *pos > cap) return -1;
    x = lpvec3_sanitize(x);
    y = lpvec3_sanitize(y);
    z = lpvec3_sanitize(z);
    double maximum = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    if (maximum < 3.051944088384301e-5) {
        if (cap - *pos < 1) return -1;
        buf[(*pos)++] = 0;
        return 0;
    }
    uint64_t scale = (uint64_t)ceil(maximum);
    bool continuation = (scale & 3u) != scale;
    uint64_t bits = (continuation ? ((scale & 3u) | 4u) : scale) |
                    (lpvec3_pack(x / (double)scale) << 3) |
                    (lpvec3_pack(y / (double)scale) << 18) |
                    (lpvec3_pack(z / (double)scale) << 33);
    if (cap - *pos < 6) return -1;
    buf[(*pos)++] = (uint8_t)bits;
    buf[(*pos)++] = (uint8_t)(bits >> 8);
    uint32_t high = (uint32_t)(bits >> 16);
    buf[(*pos)++] = (uint8_t)(high >> 24);
    buf[(*pos)++] = (uint8_t)(high >> 16);
    buf[(*pos)++] = (uint8_t)(high >> 8);
    buf[(*pos)++] = (uint8_t)high;
    if (continuation) {
        size_t written = 0;
        if (varint_write(buf + *pos, cap - *pos, (int32_t)(uint32_t)(scale >> 2), &written) != 0) return -1;
        *pos += written;
    }
    return 0;
}
