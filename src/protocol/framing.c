#include "mc_net.h"
#include "mc_protocol.h"
#include "mc_util.h"
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


static int decompress_payload(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)dst_len;
    if (inflateInit(&strm) != Z_OK) return -1;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END) return -1;
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
        return 1;
    }
    if (packet_len <= 0) return -1;
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
        if (data_len != 0) {
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
    out_frame->payload.data = (uint8_t *)malloc(payload_src_len - id_bytes);
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

    size_t uncompressed_len = id_bytes + payload_len;

    uint8_t *body = NULL;
    size_t body_len = 0;

    if (compression_threshold >= 0) {
        if ((int)uncompressed_len >= compression_threshold) {
            uLongf comp_bound = compressBound((uLong)uncompressed_len);
            uint8_t *comp = (uint8_t *)malloc(comp_bound);
            if (!comp) return -1;
            if (compress2(comp, &comp_bound, header, (uLong)id_bytes, Z_DEFAULT_COMPRESSION) != Z_OK) {
                free(comp);
                return -1;
            }
            if (compress2(comp + comp_bound, &comp_bound, payload, (uLong)payload_len, Z_DEFAULT_COMPRESSION) != Z_OK) {
                /* fall back: no compression in this stub path */
            }
            free(comp);
            /* For now, don't compress in output path to keep stub simple */
        }
        /* data_len VarInt = 0 (no compression) */
        uint8_t data_len_buf[8];
        size_t data_len_bytes = 0;
        if (varint_write(data_len_buf, sizeof(data_len_buf), 0, &data_len_bytes) != 0) return -1;
        body_len = data_len_bytes + uncompressed_len;
        body = (uint8_t *)malloc(body_len);
        if (!body) return -1;
        memcpy(body, data_len_buf, data_len_bytes);
        memcpy(body + data_len_bytes, header, id_bytes);
        memcpy(body + data_len_bytes + id_bytes, payload, payload_len);
    } else {
        body_len = uncompressed_len;
        body = (uint8_t *)malloc(body_len);
        if (!body) return -1;
        memcpy(body, header, id_bytes);
        memcpy(body + id_bytes, payload, payload_len);
    }

    uint8_t len_buf[8];
    size_t len_bytes = 0;
    if (varint_write(len_buf, sizeof(len_buf), (int32_t)body_len, &len_bytes) != 0) {
        free(body);
        return -1;
    }

    pthread_mutex_lock(&c->out_lock);
    if (buf_write(&c->out, len_buf, len_bytes) != 0) {
        pthread_mutex_unlock(&c->out_lock);
        free(body);
        return -1;
    }

    int rc = mc_crypto_write(c, body, body_len, &c->out);
    pthread_mutex_unlock(&c->out_lock);
    free(body);
    return rc;
}

void conn_close(mc_conn_t *c) {
    if (!c) return;
    c->closing = true;
}
