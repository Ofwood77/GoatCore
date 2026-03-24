#include "mc_net.h"
#include <stdlib.h>
#include <string.h>

int buf_init(mc_buf_t *b, size_t cap) {
    b->data = (uint8_t *)malloc(cap);
    if (!b->data) return -1;
    b->cap = cap;
    b->len = 0;
    b->rpos = 0;
    return 0;
}

void buf_free(mc_buf_t *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
    b->rpos = 0;
}

void buf_compact(mc_buf_t *b) {
    if (b->rpos == 0) return;
    if (b->rpos >= b->len) {
        b->len = 0;
        b->rpos = 0;
        return;
    }
    memmove(b->data, b->data + b->rpos, b->len - b->rpos);
    b->len -= b->rpos;
    b->rpos = 0;
}

int buf_reserve(mc_buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t new_cap = b->cap * 2;
    while (new_cap < b->len + need) new_cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap = new_cap;
    return 0;
}

int buf_write(mc_buf_t *b, const uint8_t *src, size_t n) {
    if (buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}
