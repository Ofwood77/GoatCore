#include "mc_net.h"
#include "mc_protocol.h"
#include "mc_util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef MC_USE_OPENSSL
int mc_crypto_read(mc_conn_t *c, uint8_t *data, size_t len);
int mc_crypto_write(mc_conn_t *c, const uint8_t *data, size_t len, mc_buf_t *out);
#else
int mc_crypto_read(mc_conn_t *c, uint8_t *data, size_t len);
int mc_crypto_write(mc_conn_t *c, const uint8_t *data, size_t len, mc_buf_t *out);
#endif

static bool debug_net_latency_enabled(void) {
    const char *env = getenv("MC_DEBUG_NET_LATENCY");
    return env && *env && strcmp(env, "0") != 0;
}

static int64_t frame_now_ms(void) {
    return mc_now_us() / 1000;
}

static int decompress_payload(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)dst_len;
    if (inflateInit(&strm) != Z_OK) return -1;
    int ret = inflate(&strm, Z_FINISH);
    bool valid = ret == Z_STREAM_END && strm.total_out == dst_len && strm.avail_in == 0;
    inflateEnd(&strm);
    if (!valid) return -1;
    return 0;
}

static int write_varint_to_tmp(uint8_t *buf, size_t cap, int32_t value, size_t *out_len) {
    if (!buf || !out_len) return -1;
    if (varint_write(buf, cap, value, out_len) != 0) return -1;
    return 0;
}

static int build_uncompressed_packet(uint8_t **out, size_t *out_len,
                                     const uint8_t *packet, size_t packet_len) {
    uint8_t data_len_buf[MC_VARINT_MAX_BYTES];
    size_t data_len_bytes = 0;
    if (!out || !out_len || !packet) return -1;
    if (write_varint_to_tmp(data_len_buf, sizeof(data_len_buf), 0, &data_len_bytes) != 0) return -1;
    if (packet_len > SIZE_MAX - data_len_bytes) return -1;

    *out_len = data_len_bytes + packet_len;
    *out = (uint8_t *)malloc(*out_len);
    if (!*out) return -1;
    memcpy(*out, data_len_buf, data_len_bytes);
    memcpy(*out + data_len_bytes, packet, packet_len);
    return 0;
}

static int build_compressed_packet(uint8_t **out, size_t *out_len,
                                   const uint8_t *packet, size_t packet_len) {
    uint8_t data_len_buf[MC_VARINT_MAX_BYTES];
    size_t data_len_bytes = 0;
    uLongf comp_bound;
    uint8_t *comp = NULL;
    int zrc;

    if (!out || !out_len || !packet) return -1;
    if (packet_len > (size_t)INT32_MAX) return -1;
    if (write_varint_to_tmp(data_len_buf, sizeof(data_len_buf), (int32_t)packet_len, &data_len_bytes) != 0) return -1;

    comp_bound = compressBound((uLong)packet_len);
    if ((uLongf)(size_t)comp_bound != comp_bound) return -1;
    comp = (uint8_t *)malloc((size_t)comp_bound);
    if (!comp) return -1;

    zrc = compress2(comp, &comp_bound, packet, (uLong)packet_len, Z_DEFAULT_COMPRESSION);
    if (zrc != Z_OK || (uLongf)(size_t)comp_bound != comp_bound) {
        free(comp);
        return -1;
    }
    if ((size_t)comp_bound > SIZE_MAX - data_len_bytes) {
        free(comp);
        return -1;
    }

    *out_len = data_len_bytes + (size_t)comp_bound;
    *out = (uint8_t *)malloc(*out_len);
    if (!*out) {
        free(comp);
        return -1;
    }
    memcpy(*out, data_len_buf, data_len_bytes);
    memcpy(*out + data_len_bytes, comp, (size_t)comp_bound);
    free(comp);
    return 0;
}

int conn_read_frame(mc_conn_t *c, mc_frame_t *out_frame, int compression_threshold) {
    if (!c || !out_frame) return -1;

    buf_compact(&c->in);
    if (c->in.len - c->in.rpos < 1) return 1; /* need more data */

    /* length VarInt */
    int32_t packet_len = 0;
    size_t len_bytes = 0;
    if (varint_read(c->in.data + c->in.rpos, c->in.len - c->in.rpos, &packet_len, &len_bytes) != 0) {
        return c->in.len - c->in.rpos >= MC_VARINT_MAX_BYTES ? -1 : 1;
    }
    if (packet_len > (int32_t)MC_MAX_FRAME || packet_len <= 0) return -1;
    if (c->in.len - c->in.rpos < len_bytes + (size_t)packet_len) return 1;

    size_t frame_start = c->in.rpos + len_bytes;
    const uint8_t *frame_data = c->in.data + frame_start;
    size_t frame_len = (size_t)packet_len;

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    const uint8_t *payload_src = frame_data;
    size_t payload_src_len = frame_len;

    if (compression_threshold >= 0) {
        int32_t data_len = 0;
        size_t data_len_bytes = 0;
        if (varint_read(frame_data, frame_len, &data_len, &data_len_bytes) != 0) return -1;
        payload_src = frame_data + data_len_bytes;
        payload_src_len = frame_len - data_len_bytes;
        if (data_len < 0 || data_len > (int32_t)MC_MAX_FRAME) return -1;
        if (data_len == 0 && payload_src_len >= (size_t)compression_threshold) return -1;
        if (data_len != 0) {
            if (data_len < compression_threshold) return -1;
            payload = (uint8_t *)malloc((size_t)data_len);
            if (!payload) return -1;
            if (decompress_payload(payload_src, payload_src_len, payload, (size_t)data_len) != 0) {
                free(payload);
                return -1;
            }
            payload_len = (size_t)data_len;
            payload_src = payload;
            payload_src_len = payload_len;
        }
    }

    if (mc_crypto_read(c, (uint8_t *)payload_src, payload_src_len) != 0) {
        free(payload);
        return -1;
    }

    int32_t packet_id = 0;
    size_t id_bytes = 0;
    if (varint_read(payload_src, payload_src_len, &packet_id, &id_bytes) != 0) {
        free(payload);
        return -1;
    }

    out_frame->packet_id = packet_id;
    out_frame->payload.data = (uint8_t *)malloc(payload_src_len - id_bytes + 1);
    if (!out_frame->payload.data) {
        free(payload);
        return -1;
    }
    out_frame->payload.len = payload_src_len - id_bytes;
    out_frame->payload.cap = out_frame->payload.len;
    out_frame->payload.rpos = 0;
    memcpy(out_frame->payload.data, payload_src + id_bytes, out_frame->payload.len);

    free(payload);

    c->in.rpos = frame_start + frame_len;
    return 0;
}

int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload, size_t payload_len, int compression_threshold) {
    uint8_t header[16];
    size_t id_bytes = 0;
    if (varint_write(header, sizeof(header), packet_id, &id_bytes) != 0) return -1;
    if (!c || (!payload && payload_len > 0)) return -1;

    size_t uncompressed_len = id_bytes + payload_len;
    if (uncompressed_len < id_bytes || uncompressed_len > MC_MAX_FRAME) return -1;

    uint8_t *packet = NULL;
    uint8_t *body = NULL;
    size_t body_len = 0;

    packet = (uint8_t *)malloc(uncompressed_len ? uncompressed_len : 1u);
    if (!packet) return -1;
    memcpy(packet, header, id_bytes);
    if (payload_len > 0) memcpy(packet + id_bytes, payload, payload_len);

    if (compression_threshold >= 0) {
        if (uncompressed_len >= (size_t)compression_threshold) {
            if (build_compressed_packet(&body, &body_len, packet, uncompressed_len) != 0) {
                free(packet);
                return -1;
            }
        } else if (build_uncompressed_packet(&body, &body_len, packet, uncompressed_len) != 0) {
            free(packet);
            return -1;
        }
    } else {
        body_len = uncompressed_len;
        body = packet;
        packet = NULL;
    }
    free(packet);
    if (body_len > MC_MAX_FRAME) {
        free(body);
        return -1;
    }

    uint8_t len_buf[8];
    size_t len_bytes = 0;
    if (varint_write(len_buf, sizeof(len_buf), (int32_t)body_len, &len_bytes) != 0) {
        free(body);
        return -1;
    }

    size_t out_before = 0;
    size_t out_after = 0;
    pthread_mutex_lock(&c->out_lock);
    buf_compact(&c->out);
    if (c->abort_io || c->out.len > MC_MAX_OUTPUT_BUFFER ||
        len_bytes + body_len > MC_MAX_OUTPUT_BUFFER - c->out.len ||
        buf_reserve(&c->out, len_bytes + body_len) != 0) {
        c->abort_io = true;
        c->closing = true;
        pthread_mutex_unlock(&c->out_lock);
        free(body);
        return -1;
    }
    out_before = c->out.len > c->out.rpos ? c->out.len - c->out.rpos : 0;
    if (buf_write(&c->out, len_buf, len_bytes) != 0) {
        pthread_mutex_unlock(&c->out_lock);
        free(body);
        return -1;
    }

    int rc = mc_crypto_write(c, body, body_len, &c->out);
    out_after = c->out.len > c->out.rpos ? c->out.len - c->out.rpos : 0;
    pthread_mutex_unlock(&c->out_lock);
    if (debug_net_latency_enabled()) {
        log_info("net latency: t=%lld queue_out fd=%d player=%s state=%d packet=0x%02X payload=%zu framed=%zu out_before=%zu out_after=%zu rc=%d",
                 (long long)frame_now_ms(), c->fd, c->username[0] ? c->username : "(unknown)",
                 atomic_load(&c->state), packet_id, payload_len, len_bytes + body_len, out_before, out_after, rc);
    }
    free(body);
    return rc;
}

void conn_close(mc_conn_t *c) {
    if (!c) return;
    c->closing = true;
}
