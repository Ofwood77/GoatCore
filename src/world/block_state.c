#include "mc_block_state.h"
#include <stdio.h>
#include <string.h>

const mc_block_desc_t *mc_block_desc_by_name(const char *name) {
    if (!name) return NULL;
    size_t lo = 0, hi = GLOBAL_BLOCK_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, GLOBAL_BLOCKS[mid].name);
        if (!cmp) return &GLOBAL_BLOCKS[mid];
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

const mc_block_desc_t *mc_block_state_desc(int32_t state) {
    if (state < 0 || (size_t)state >= GLOBAL_BLOCK_STATES_COUNT ||
        !(GLOBAL_BLOCK_STATES[state].flags & MC_BLOCK_FLAG_VALID)) return NULL;
    return &GLOBAL_BLOCKS[GLOBAL_BLOCK_STATES[state].block_index];
}

static const char *property_start(int32_t state, const char *name) {
    if (!name || !*name || !mc_block_state_desc(state)) return NULL;
    const char *key = mc_global_state_key((uint32_t)state);
    const char *p = strchr(key, '[');
    size_t n = strlen(name);
    while (p && *p != ']') {
        p++;
        if (strncmp(p, name, n) == 0 && p[n] == '=') return p + n + 1;
        p = strpbrk(p, ",]");
    }
    return NULL;
}

bool mc_block_state_property(int32_t state, const char *name, char *value, size_t capacity) {
    const char *p = property_start(state, name);
    if (!p || !value || !capacity) return false;
    size_t n = strcspn(p, ",]");
    if (n >= capacity) return false;
    memcpy(value, p, n);
    value[n] = '\0';
    return true;
}

int32_t mc_block_state_with_property(int32_t state, const char *name, const char *value) {
    const char *p = property_start(state, name);
    if (!p || !value || !*value || strpbrk(value, ",[]= ")) return -1;
    const char *key = mc_global_state_key((uint32_t)state);
    char next[512];
    size_t prefix = (size_t)(p - key);
    int n = snprintf(next, sizeof(next), "%.*s%s%s", (int)prefix, key, value, p + strcspn(p, ",]"));
    if (n < 0 || (size_t)n >= sizeof(next)) return -1;
    uint32_t result = mc_global_state_id_from_key(next, UINT32_MAX);
    return result == UINT32_MAX ? -1 : (int32_t)result;
}
