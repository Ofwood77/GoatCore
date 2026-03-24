#include "mc_protocol.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
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

static int r_string_skip(mc_reader_t *r) {
    int32_t str_len = 0;
    if (r_varint(r, &str_len) != 0) return -1;
    if (str_len < 0 || (size_t)str_len > r->len - r->pos) return -1;
    r->pos += (size_t)str_len;
    return 0;
}

static int w_varint(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    size_t n = 0;
    if (varint_write(buf + *pos, cap - *pos, v, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int w_string(uint8_t *buf, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    if (w_varint(buf, cap, pos, (int32_t)len) != 0) return -1;
    if (*pos + len > cap) return -1;
    memcpy(buf + *pos, s, len);
    *pos += len;
    return 0;
}

static int send_config_disconnect(mc_conn_t *c, const char *reason_json) {
    const char *msg = reason_json ? reason_json : "{\"text\":\"Configuration error\"}";
    size_t msg_len = strlen(msg);

    uint8_t buf[512];
    size_t pos = 0;
    size_t str_len_bytes = 0;
    if (varint_write(buf + pos, sizeof(buf) - pos, (int32_t)msg_len, &str_len_bytes) != 0) return -1;
    pos += str_len_bytes;
    if (pos + msg_len > sizeof(buf)) return -1;
    memcpy(buf + pos, msg, msg_len);
    pos += msg_len;

    if (conn_write_packet(c, MC_PKT_CONFIGURATION_CLIENTBOUND_DISCONNECT, buf, pos, -1) != 0) return -1;
    c->closing = true;
    return 0;
}

int proto_config_send_known_packs(mc_conn_t *c) {
    if (!c) return -1;
    if (c->config_known_packs_sent) return 0;

    uint8_t buf[256];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, 2) != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "minecraft") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "core") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, MC_GAME_VERSION) != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "minecraft") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "vanilla") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, MC_GAME_VERSION) != 0) return -1;

    if (conn_write_packet(c, MC_PKT_CONFIGURATION_CLIENTBOUND_SELECT_KNOWN_PACKS, buf, pos, -1) != 0) return -1;
    c->config_known_packs_sent = true;
    return 0;
}

int proto_config_send_registry(mc_conn_t *c) {
    if (!c || !c->cfg) return -1;
    if (c->config_registry_sent) return 0;

    const char *path = c->cfg->registry_blob_path;
    if (!path) {
        log_error("registry blob path not configured");
        send_config_disconnect(c, "{\"text\":\"Registry data missing\"}");
        return -1;
    }

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (read_file(path, &blob, &blob_len) != 0) {
        log_error("failed to read registry blob: %s", path);
        send_config_disconnect(c, "{\"text\":\"Registry data load failed\"}");
        return -1;
    }

    size_t pos = 0;
    int packets = 0;
    while (pos < blob_len) {
        int32_t payload_len = 0;
        size_t n = 0;
        if (varint_read(blob + pos, blob_len - pos, &payload_len, &n) != 0) {
            free(blob);
            log_error("registry blob parse error (varint)");
            send_config_disconnect(c, "{\"text\":\"Registry data parse error\"}");
            return -1;
        }
        pos += n;
        if (payload_len <= 0 || pos + (size_t)payload_len > blob_len) {
            free(blob);
            log_error("registry blob parse error (length)");
            send_config_disconnect(c, "{\"text\":\"Registry data parse error\"}");
            return -1;
        }
        if (conn_write_packet(c, MC_PKT_CONFIGURATION_CLIENTBOUND_REGISTRY_DATA, blob + pos, (size_t)payload_len, -1) != 0) {
            free(blob);
            return -1;
        }
        pos += (size_t)payload_len;
        packets++;
    }
    free(blob);
    log_info("sent %d registry packets", packets);
    c->config_registry_sent = true;
    return 0;
}

int proto_config_handle(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame) return -1;

    if (!c->config_known_packs_sent) {
        if (proto_config_send_known_packs(c) != 0) return -1;
    }

    if (frame->packet_id == MC_PKT_CONFIGURATION_SERVERBOUND_SELECT_KNOWN_PACKS) {
        mc_reader_t r;
        r.data = frame->payload.data;
        r.len = frame->payload.len;
        r.pos = 0;

        int32_t count = 0;
        if (r_varint(&r, &count) != 0) return -1;
        for (int32_t i = 0; i < count; i++) {
            if (r_string_skip(&r) != 0) return -1;
            if (r_string_skip(&r) != 0) return -1;
            if (r_string_skip(&r) != 0) return -1;
        }

        if (proto_config_send_registry(c) != 0) return -1;

        if (!c->config_finish_sent) {
            if (conn_write_packet(c, MC_PKT_CONFIGURATION_CLIENTBOUND_FINISH_CONFIGURATION, NULL, 0, -1) != 0) return -1;
            c->config_finish_sent = true;
        }
        return 0;
    }

    if (frame->packet_id == MC_PKT_CONFIGURATION_SERVERBOUND_FINISH_CONFIGURATION) {
        if (!c->config_finish_sent) return -1;
        c->state = MC_STATE_PLAY;
        return proto_play_send_initial(c);
    }

    return 0;
}
