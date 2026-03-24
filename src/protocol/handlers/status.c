#include "mc_protocol.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int proto_handle_status(mc_conn_t *c, const mc_frame_t *frame, const char *motd_json, int32_t online_players, int32_t max_players) {
    if (!c || !frame) return -1;

    if (frame->packet_id == MC_PKT_STATUS_SERVERBOUND_STATUS_REQUEST) {
        /* Status Request */
        const char *motd_text = motd_json ? motd_json : "C server stub";
        char motd[512];
        int wrote = snprintf(
            motd,
            sizeof(motd),
            "{\"version\":{\"name\":\"%s\",\"protocol\":767},"
            "\"players\":{\"max\":%d,\"online\":%d},"
            "\"description\":{\"text\":\"%s\"}}",
            MC_GAME_VERSION,
            max_players,
            online_players,
            motd_text
        );
        if (wrote <= 0 || (size_t)wrote >= sizeof(motd)) return -1;
        size_t motd_len = (size_t)wrote;

        uint8_t buf[1024];
        size_t pos = 0;

        size_t str_len_bytes = 0;
        if (varint_write(buf + pos, sizeof(buf) - pos, (int32_t)motd_len, &str_len_bytes) != 0) return -1;
        pos += str_len_bytes;
        if (pos + motd_len > sizeof(buf)) return -1;
        memcpy(buf + pos, motd, motd_len);
        pos += motd_len;

        return conn_write_packet(c, MC_PKT_STATUS_CLIENTBOUND_STATUS_RESPONSE, buf, pos, -1);
    } else if (frame->packet_id == MC_PKT_STATUS_SERVERBOUND_PING_REQUEST) {
        /* Ping: payload is 8 bytes */
        if (frame->payload.len != 8) return -1;
        if (conn_write_packet(c, MC_PKT_STATUS_CLIENTBOUND_PONG_RESPONSE, frame->payload.data, frame->payload.len, -1) != 0) return -1;
        c->closing = true;
        return 0;
    }

    return -1;
}
