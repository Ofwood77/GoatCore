#include "mc_net.h"
#include "mc_protocol.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void test_conn_init(mc_conn_t *c) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    atomic_init(&c->state, MC_STATE_PLAY);
    atomic_init(&c->closing, false);
    atomic_init(&c->refcount, 1);
    assert(pthread_mutex_init(&c->out_lock, NULL) == 0);
    assert(buf_init(&c->in, 64) == 0);
    assert(buf_init(&c->out, 64) == 0);
}

static void test_conn_clear(mc_conn_t *c) {
    buf_free(&c->in);
    buf_free(&c->out);
    pthread_mutex_destroy(&c->out_lock);
}

static int read_varint_at(const uint8_t *buf, size_t len, size_t *pos, int32_t *out) {
    size_t n = 0;
    if (!buf || !pos || !out || *pos > len) return -1;
    if (varint_read(buf + *pos, len - *pos, out, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static void read_frame_prefix(const mc_buf_t *out, int32_t *packet_len, size_t *body_pos) {
    size_t pos = 0;
    assert(read_varint_at(out->data, out->len, &pos, packet_len) == 0);
    assert(*packet_len > 0);
    assert(pos + (size_t)*packet_len == out->len);
    *body_pos = pos;
}

static void assert_raw_packet(const uint8_t *packet, size_t packet_len, int32_t expected_id,
                              const uint8_t *payload, size_t payload_len) {
    size_t pos = 0;
    int32_t packet_id = 0;
    assert(read_varint_at(packet, packet_len, &pos, &packet_id) == 0);
    assert(packet_id == expected_id);
    assert(packet_len - pos == payload_len);
    assert(memcmp(packet + pos, payload, payload_len) == 0);
}

static void test_no_compression(void) {
    mc_conn_t c;
    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    int32_t packet_len = 0;
    size_t body_pos = 0;

    test_conn_init(&c);
    assert(conn_write_packet(&c, 0x23, payload, sizeof(payload), -1) == 0);
    read_frame_prefix(&c.out, &packet_len, &body_pos);
    assert_raw_packet(c.out.data + body_pos, (size_t)packet_len, 0x23, payload, sizeof(payload));
    test_conn_clear(&c);
}

static void test_below_threshold_uncompressed(void) {
    mc_conn_t c;
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
    int32_t packet_len = 0;
    int32_t data_len = -1;
    size_t pos = 0;

    test_conn_init(&c);
    assert(conn_write_packet(&c, 0x05, payload, sizeof(payload), 64) == 0);
    read_frame_prefix(&c.out, &packet_len, &pos);
    assert(read_varint_at(c.out.data, c.out.len, &pos, &data_len) == 0);
    assert(data_len == 0);
    assert_raw_packet(c.out.data + pos, c.out.len - pos, 0x05, payload, sizeof(payload));
    test_conn_clear(&c);
}

static void test_above_threshold_compressed(void) {
    mc_conn_t c;
    uint8_t payload[256];
    uint8_t inflated[320];
    int32_t packet_len = 0;
    int32_t data_len = -1;
    size_t pos = 0;
    uLongf inflated_len = sizeof(inflated);

    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)('A' + (i % 5));

    test_conn_init(&c);
    assert(conn_write_packet(&c, 0x2A, payload, sizeof(payload), 16) == 0);
    read_frame_prefix(&c.out, &packet_len, &pos);
    assert(read_varint_at(c.out.data, c.out.len, &pos, &data_len) == 0);
    assert(data_len > (int32_t)sizeof(payload));
    assert(uncompress(inflated, &inflated_len, c.out.data + pos, (uLong)(c.out.len - pos)) == Z_OK);
    assert(inflated_len == (uLongf)data_len);
    assert_raw_packet(inflated, (size_t)inflated_len, 0x2A, payload, sizeof(payload));
    test_conn_clear(&c);
}

static void test_compressed_roundtrip_read(void) {
    mc_conn_t writer;
    mc_conn_t reader;
    uint8_t payload[128];
    mc_frame_t frame;

    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i & 0xFF);

    test_conn_init(&writer);
    test_conn_init(&reader);
    assert(conn_write_packet(&writer, 0x33, payload, sizeof(payload), 8) == 0);
    assert(buf_write(&reader.in, writer.out.data, writer.out.len) == 0);
    memset(&frame, 0, sizeof(frame));
    assert(conn_read_frame(&reader, &frame, 8) == 0);
    assert(frame.packet_id == 0x33);
    assert(frame.payload.len == sizeof(payload));
    assert(memcmp(frame.payload.data, payload, sizeof(payload)) == 0);
    free(frame.payload.data);
    test_conn_clear(&reader);
    test_conn_clear(&writer);
}

static void test_partial_frame_needs_more_data(void) {
    mc_conn_t c;
    mc_frame_t frame;
    uint8_t partial[] = {0x05, 0x01, 0x02};

    test_conn_init(&c);
    assert(buf_write(&c.in, partial, sizeof(partial)) == 0);
    memset(&frame, 0, sizeof(frame));
    assert(conn_read_frame(&c, &frame, -1) == 1);
    assert(c.in.rpos == 0);
    test_conn_clear(&c);
}

int main(void) {
    test_no_compression();
    test_below_threshold_uncompressed();
    test_above_threshold_compressed();
    test_compressed_roundtrip_read();
    test_partial_frame_needs_more_data();
    return 0;
}
