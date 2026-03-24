#ifndef STB_PERLIN_H
#define STB_PERLIN_H

/*
 * Minimal stb_perlin-compatible header (subset).
 *
 * We only expose stb_perlin_noise3_seed() for world generation.
 * This is a small, fast Perlin noise implementation with a stable
 * seeded hash (no global state, no dynamic allocation).
 *
 * API reference (subset):
 *   float stb_perlin_noise3_seed(float x, float y, float z,
 *                                int x_wrap, int y_wrap, int z_wrap,
 *                                int seed);
 */

#include <math.h>
#include <stdint.h>

static inline uint32_t stb__hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static inline uint32_t stb__hash_coords(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t hx = (uint32_t)x * 374761393U;
    uint32_t hy = (uint32_t)y * 668265263U;
    uint32_t hz = (uint32_t)z * 2246822519U;
    uint32_t h = hx ^ hy ^ hz ^ (seed * 3266489917U);
    return stb__hash_u32(h);
}

static inline float stb__fade(float t) {
    /* 6t^5 - 15t^4 + 10t^3 */
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline float stb__lerp(float a, float b, float t) {
    return a + t * (b - a);
}

static inline float stb__grad(uint32_t hash, float x, float y, float z) {
    /* Classic Perlin grad mapping (Ken Perlin reference). */
    int h = (int)(hash & 15u);
    float u = (h < 8) ? x : y;
    float v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    float a = (h & 1) ? -u : u;
    float b = (h & 2) ? -v : v;
    return a + b;
}

static inline int32_t stb__floor_i32(float x) {
    int32_t i = (int32_t)x;
    return (x < (float)i) ? (i - 1) : i;
}

static inline int32_t stb__wrap_i32(int32_t v, int wrap) {
    if (wrap <= 0) return v;
    int32_t m = v % wrap;
    if (m < 0) m += wrap;
    return m;
}

static inline float stb_perlin_noise3_seed(float x, float y, float z, int x_wrap, int y_wrap, int z_wrap, int seed) {
    int32_t xi0 = stb__floor_i32(x);
    int32_t yi0 = stb__floor_i32(y);
    int32_t zi0 = stb__floor_i32(z);
    int32_t xi1 = xi0 + 1;
    int32_t yi1 = yi0 + 1;
    int32_t zi1 = zi0 + 1;

    float xf = x - (float)xi0;
    float yf = y - (float)yi0;
    float zf = z - (float)zi0;

    xi0 = stb__wrap_i32(xi0, x_wrap);
    yi0 = stb__wrap_i32(yi0, y_wrap);
    zi0 = stb__wrap_i32(zi0, z_wrap);
    xi1 = stb__wrap_i32(xi1, x_wrap);
    yi1 = stb__wrap_i32(yi1, y_wrap);
    zi1 = stb__wrap_i32(zi1, z_wrap);

    uint32_t s = (uint32_t)seed;

    uint32_t h000 = stb__hash_coords(xi0, yi0, zi0, s);
    uint32_t h100 = stb__hash_coords(xi1, yi0, zi0, s);
    uint32_t h010 = stb__hash_coords(xi0, yi1, zi0, s);
    uint32_t h110 = stb__hash_coords(xi1, yi1, zi0, s);
    uint32_t h001 = stb__hash_coords(xi0, yi0, zi1, s);
    uint32_t h101 = stb__hash_coords(xi1, yi0, zi1, s);
    uint32_t h011 = stb__hash_coords(xi0, yi1, zi1, s);
    uint32_t h111 = stb__hash_coords(xi1, yi1, zi1, s);

    float u = stb__fade(xf);
    float v = stb__fade(yf);
    float w = stb__fade(zf);

    float n000 = stb__grad(h000, xf, yf, zf);
    float n100 = stb__grad(h100, xf - 1.0f, yf, zf);
    float n010 = stb__grad(h010, xf, yf - 1.0f, zf);
    float n110 = stb__grad(h110, xf - 1.0f, yf - 1.0f, zf);
    float n001 = stb__grad(h001, xf, yf, zf - 1.0f);
    float n101 = stb__grad(h101, xf - 1.0f, yf, zf - 1.0f);
    float n011 = stb__grad(h011, xf, yf - 1.0f, zf - 1.0f);
    float n111 = stb__grad(h111, xf - 1.0f, yf - 1.0f, zf - 1.0f);

    float x00 = stb__lerp(n000, n100, u);
    float x10 = stb__lerp(n010, n110, u);
    float x01 = stb__lerp(n001, n101, u);
    float x11 = stb__lerp(n011, n111, u);

    float y0 = stb__lerp(x00, x10, v);
    float y1 = stb__lerp(x01, x11, v);

    float n = stb__lerp(y0, y1, w);
    /* Empirically, this stays in ~[-1,1]. */
    return n;
}

#endif /* STB_PERLIN_H */

