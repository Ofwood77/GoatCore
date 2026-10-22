#include "mc_protocol.h"

#include <limits.h>
#include <string.h>

size_t proto_profile_properties_capacity(const mc_conn_t *c) {
    if (!c || !c->skin_value || !c->skin_signature) return 5;
    return 64 + strlen(c->skin_value) + strlen(c->skin_signature);
}

static int write_string(uint8_t *buf, size_t cap, size_t *pos, const char *str) {
    size_t len = strlen(str), count = 0;
    if (len > INT32_MAX || *pos > cap || varint_write(buf + *pos, cap - *pos, (int32_t)len, &count) != 0) return -1;
    *pos += count;
    if (len > cap - *pos) return -1;
    memcpy(buf + *pos, str, len);
    *pos += len;
    return 0;
}

int proto_profile_write_properties(const mc_conn_t *c, uint8_t *buf, size_t cap, size_t *pos) {
    if (!c || !buf || !pos || *pos > cap) return -1;
    bool have_skin = c->skin_value && c->skin_signature;
    size_t count = 0;
    if (varint_write(buf + *pos, cap - *pos, have_skin ? 1 : 0, &count) != 0) return -1;
    *pos += count;
    if (!have_skin) return 0;
    if (write_string(buf, cap, pos, "textures") != 0 ||
        write_string(buf, cap, pos, c->skin_value) != 0 || *pos >= cap) return -1;
    buf[(*pos)++] = 1; /* signed property */
    return write_string(buf, cap, pos, c->skin_signature);
}
