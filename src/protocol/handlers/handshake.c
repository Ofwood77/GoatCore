#include "mc_protocol.h"
#include "mc_util.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mc_reader_t;

static int r_varint(mc_reader_t *r, int32_t *out) {
    size_t n = 0;
    if (r->pos >= r->len) return -1;
    if (varint_read(r->data + r->pos, r->len - r->pos, out, &n) != 0) return -1;
    r->pos += n;
    return 0;
}

static int r_u16(mc_reader_t *r, uint16_t *out) {
    if (r->pos + 2 > r->len) return -1;
    *out = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return 0;
}

static int r_string(mc_reader_t *r, char *out, size_t out_cap) {
    int32_t str_len = 0;
    if (r_varint(r, &str_len) != 0) return -1;
    if (str_len < 0 || (size_t)str_len > r->len - r->pos) return -1;
    size_t copy_len = (size_t)str_len;
    if (copy_len >= out_cap) copy_len = out_cap - 1;
    memcpy(out, r->data + r->pos, copy_len);
    out[copy_len] = '\0';
    r->pos += (size_t)str_len;
    return 0;
}

int proto_handle_handshake(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame) return -1;

    mc_reader_t r;
    r.data = frame->payload.data;
    r.len = frame->payload.len;
    r.pos = 0;

    int32_t proto_ver = 0;
    if (r_varint(&r, &proto_ver) != 0) return -1;

    char server_addr[256];
    if (r_string(&r, server_addr, sizeof(server_addr)) != 0) return -1;

    uint16_t server_port = 0;
    if (r_u16(&r, &server_port) != 0) return -1;

    int32_t next_state = 0;
    if (r_varint(&r, &next_state) != 0) return -1;

    log_info("Handshake: proto=%d addr=%s port=%u next=%d", proto_ver, server_addr, server_port, next_state);

    if (next_state == 1) c->state = MC_STATE_STATUS;
    else if (next_state == 2) c->state = MC_STATE_LOGIN;
    else {
        log_error("Invalid next_state=%d, dumping payload", next_state);
        hex_dump(frame->payload.data, frame->payload.len);
        return -1;
    }

    return 0;
}
