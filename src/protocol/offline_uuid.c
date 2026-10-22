#include "mc_protocol.h"

#include <stdint.h>
#include <string.h>

/* Java UUID.nameUUIDFromBytes(MD5("OfflinePlayer:" + name)), for the
 * protocol's ASCII player names. The input is at most 30 bytes, so MD5
 * always uses exactly one 64-byte block. This is not used for security. */
void proto_fill_offline_uuid(const char *username, uint8_t out[16]) {
    static const uint32_t shifts[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    static const uint32_t constants[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    uint8_t block[64] = {0};
    uint32_t words[16];
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const char prefix[] = "OfflinePlayer:";
    size_t name_len = username ? strlen(username) : 0;
    if (name_len > 16) name_len = 16;
    size_t len = sizeof(prefix) - 1 + name_len;
    memcpy(block, prefix, sizeof(prefix) - 1);
    if (name_len) memcpy(block + sizeof(prefix) - 1, username, name_len);
    block[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bit_len >> (8 * i));
    for (int i = 0; i < 16; i++) {
        words[i] = (uint32_t)block[4 * i] | ((uint32_t)block[4 * i + 1] << 8) |
                   ((uint32_t)block[4 * i + 2] << 16) | ((uint32_t)block[4 * i + 3] << 24);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, index;
        if (i < 16) { f = (b & c) | (~b & d); index = i; }
        else if (i < 32) { f = (d & b) | (~d & c); index = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; index = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); index = (7 * i) & 15; }
        uint32_t sum = a + f + constants[i] + words[index];
        uint32_t rotated = (sum << shifts[i]) | (sum >> (32 - shifts[i]));
        a = d; d = c; c = b; b += rotated;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(state[i] >> (8 * j));
    }
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x30);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
}

static uint64_t fnv1a64(const uint8_t *data, size_t len, uint64_t seed) {
    for (size_t i = 0; i < len; i++) {
        seed ^= data[i];
        seed *= 1099511628211ULL;
    }
    return seed;
}

/* Read-only key for saves written by GoatCore before the UUID correction. */
void proto_fill_legacy_offline_uuid(const char *username, uint8_t out[16]) {
    static const char prefix[] = "OfflinePlayer:";
    uint64_t a = fnv1a64((const uint8_t *)prefix, sizeof(prefix) - 1, 1469598103934665603ULL);
    uint64_t b = fnv1a64((const uint8_t *)prefix, sizeof(prefix) - 1, 1099511628211ULL);
    if (username) {
        a = fnv1a64((const uint8_t *)username, strlen(username), a);
        b = fnv1a64((const uint8_t *)username, strlen(username), b);
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(a >> (56 - i * 8));
        out[8 + i] = (uint8_t)(b >> (56 - i * 8));
    }
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x30);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
}
