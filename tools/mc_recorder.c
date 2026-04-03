#include "mc_protocol.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#define STATE_LOGIN 0
#define STATE_CONFIG 1
#define STATE_PLAY 2

#define PKT_LOGIN_SUCCESS MC_PKT_LOGIN_CLIENTBOUND_LOGIN_FINISHED
#define PKT_LOGIN_ACK MC_PKT_LOGIN_SERVERBOUND_LOGIN_ACKNOWLEDGED
#define PKT_CONFIG_KNOWN_PACKS_CB MC_PKT_CONFIGURATION_CLIENTBOUND_SELECT_KNOWN_PACKS
#define PKT_CONFIG_KNOWN_PACKS_SB MC_PKT_CONFIGURATION_SERVERBOUND_SELECT_KNOWN_PACKS
#define PKT_CONFIG_REGISTRY_DATA MC_PKT_CONFIGURATION_CLIENTBOUND_REGISTRY_DATA
#define PKT_CONFIG_UPDATE_TAGS MC_PKT_CONFIGURATION_CLIENTBOUND_UPDATE_TAGS
#define PKT_CONFIG_FINISH_CB MC_PKT_CONFIGURATION_CLIENTBOUND_FINISH_CONFIGURATION
#define PKT_CONFIG_FINISH_ACK MC_PKT_CONFIGURATION_SERVERBOUND_FINISH_CONFIGURATION

#define PKT_CONFIG_KEEPALIVE_CB MC_PKT_CONFIGURATION_CLIENTBOUND_KEEP_ALIVE
#define PKT_CONFIG_KEEPALIVE_SB MC_PKT_CONFIGURATION_SERVERBOUND_KEEP_ALIVE
#define PKT_PLAY_KEEPALIVE_CB MC_PKT_PLAY_CLIENTBOUND_KEEP_ALIVE
#define PKT_PLAY_KEEPALIVE_SB MC_PKT_PLAY_SERVERBOUND_KEEP_ALIVE
#define PKT_PLAY_SYNC_POS MC_PKT_PLAY_CLIENTBOUND_PLAYER_POSITION
#define PKT_PLAY_CONFIRM_TELEPORT MC_PKT_PLAY_SERVERBOUND_ACCEPT_TELEPORTATION
#define PKT_PLAY_CHUNK_DATA MC_PKT_PLAY_CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT

static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int flush_out(mc_conn_t *c) {
    while (c->out.rpos < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out.rpos, c->out.len - c->out.rpos, 0);
        if (n <= 0) return -1;
        c->out.rpos += (size_t)n;
    }
    buf_compact(&c->out);
    return 0;
}

static int send_packet(mc_conn_t *c, int32_t id, const uint8_t *payload, size_t len, int compression_threshold) {
    if (conn_write_packet(c, id, payload, len, compression_threshold) != 0) return -1;
    return flush_out(c);
}

static int write_varint_to_file(FILE *f, int32_t v) {
    uint8_t buf[8];
    size_t n = 0;
    if (varint_write(buf, sizeof(buf), v, &n) != 0) return -1;
    if (fwrite(buf, 1, n, f) != n) return -1;
    return 0;
}

static int send_handshake(mc_conn_t *c, const char *host, uint16_t port) {
    uint8_t buf[512];
    size_t pos = 0;

    size_t n = 0;
    if (varint_write(buf + pos, sizeof(buf) - pos, MC_PROTO_VERSION_1_21_1, &n) != 0) return -1;
    pos += n;

    size_t host_len = strlen(host);
    if (varint_write(buf + pos, sizeof(buf) - pos, (int32_t)host_len, &n) != 0) return -1;
    pos += n;
    if (pos + host_len + 2 + 1 > sizeof(buf)) return -1;
    memcpy(buf + pos, host, host_len);
    pos += host_len;

    buf[pos++] = (uint8_t)((port >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(port & 0xFF);

    if (varint_write(buf + pos, sizeof(buf) - pos, 2, &n) != 0) return -1; /* next state login */
    pos += n;

    return send_packet(c, MC_PKT_HANDSHAKING_CLIENT_INTENTION, buf, pos, -1);
}

static int send_login_start(mc_conn_t *c, const char *username, int compression_threshold) {
    uint8_t buf[64];
    size_t pos = 0;
    size_t n = 0;
    size_t name_len = strlen(username);
    if (varint_write(buf + pos, sizeof(buf) - pos, (int32_t)name_len, &n) != 0) return -1;
    pos += n;
    if (pos + name_len > sizeof(buf)) return -1;
    memcpy(buf + pos, username, name_len);
    pos += name_len;
    /* UUID (16 bytes). For offline recorder, send zeros. */
    if (pos + 16 > sizeof(buf)) return -1;
    memset(buf + pos, 0, 16);
    pos += 16;
    return send_packet(c, MC_PKT_LOGIN_SERVERBOUND_HELLO, buf, pos, compression_threshold);
}

static int handle_sync_pos(mc_conn_t *c, const mc_frame_t *frame) {
    if (frame->payload.len < (8 * 3 + 4 * 2 + 1)) return -1;
    size_t pos = 8 * 3 + 4 * 2 + 1;
    int32_t teleport_id = 0;
    size_t n = 0;
    if (varint_read(frame->payload.data + pos, frame->payload.len - pos, &teleport_id, &n) != 0) return -1;
    uint8_t buf[8];
    size_t w = 0;
    if (varint_write(buf, sizeof(buf), teleport_id, &w) != 0) return -1;
    return send_packet(c, PKT_PLAY_CONFIRM_TELEPORT, buf, w, -1);
}

static int read_string_payload(const uint8_t *data, size_t len, char *out, size_t out_cap) {
    int32_t slen = 0;
    size_t n = 0;
    if (varint_read(data, len, &slen, &n) != 0) return -1;
    if (slen < 0 || (size_t)slen > len - n) return -1;
    size_t copy = (size_t)slen;
    if (copy >= out_cap) copy = out_cap - 1;
    memcpy(out, data + n, copy);
    out[copy] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <host> <port> <registry_out> <tags_out> <chunk_out> [username]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *registry_out = argv[3];
    const char *tags_out = argv[4];
    const char *chunk_out = argv[5];
    const char *username = (argc >= 7) ? argv[6] : "Recorder";

    int registry_packets = 0;
    bool tags_written = false;
    bool chunk_written = false;

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "failed to connect to %s:%s\n", host, port);
        return 1;
    }
    fprintf(stdout, "connected to %s:%s\n", host, port);

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    FILE *reg = fopen(registry_out, "wb");
    if (!reg) {
        fprintf(stderr, "failed to open registry output\n");
        close(fd);
        return 1;
    }
    FILE *tags = fopen(tags_out, "wb");
    if (!tags) {
        fprintf(stderr, "failed to open tags output\n");
        fclose(reg);
        close(fd);
        return 1;
    }
    FILE *chunk = fopen(chunk_out, "wb");
    if (!chunk) {
        fprintf(stderr, "failed to open chunk output\n");
        fclose(tags);
        fclose(reg);
        close(fd);
        return 1;
    }

    mc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;
    if (buf_init(&c.in, MC_BUF_CAP) != 0 || buf_init(&c.out, MC_BUF_CAP) != 0) {
        fclose(tags);
        fclose(reg);
        fclose(chunk);
        close(fd);
        return 1;
    }

    int compression_threshold = -1;

    if (send_handshake(&c, host, (uint16_t)atoi(port)) != 0) {
        fprintf(stderr, "failed to send handshake\n");
        goto cleanup;
    }
    if (send_login_start(&c, username, compression_threshold) != 0) {
        fprintf(stderr, "failed to send login start\n");
        goto cleanup;
    }
    fprintf(stdout, "sent handshake + login start\n");

    int state = STATE_LOGIN;

    for (;;) {
        uint8_t tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            fprintf(stderr, "connection closed by server\n");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "recv timeout (no data)\n");
            } else {
                fprintf(stderr, "recv error: %s\n", strerror(errno));
            }
            break;
        }
        if (buf_write(&c.in, tmp, (size_t)n) != 0) break;

        for (;;) {
            mc_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            int rc = conn_read_frame(&c, &frame, compression_threshold);
            if (rc == 1) break;
            if (rc != 0) goto cleanup;

            fprintf(stdout, "recv state=%d id=0x%02X len=%zu\n", state, frame.packet_id, frame.payload.len);

            if (state == STATE_LOGIN) {
                if (frame.packet_id == 0x03) {
                    int32_t threshold = 0;
                    size_t n = 0;
                    if (varint_read(frame.payload.data, frame.payload.len, &threshold, &n) == 0) {
                        compression_threshold = threshold;
                        fprintf(stdout, "set compression threshold=%d\n", threshold);
                    }
                }
                if (frame.packet_id == 0x00) {
                    char reason[256];
                    if (read_string_payload(frame.payload.data, frame.payload.len, reason, sizeof(reason)) == 0) {
                        fprintf(stderr, "login disconnect: %s\n", reason);
                    } else {
                        fprintf(stderr, "login disconnect (unparsed)\n");
                    }
                    goto cleanup;
                }
                if (frame.packet_id == PKT_LOGIN_SUCCESS) {
                    if (send_packet(&c, PKT_LOGIN_ACK, NULL, 0, compression_threshold) != 0) goto cleanup;
                    state = STATE_CONFIG;
                }
            } else if (state == STATE_CONFIG) {
                if (frame.packet_id == PKT_CONFIG_KNOWN_PACKS_CB) {
                    if (send_packet(&c, PKT_CONFIG_KNOWN_PACKS_SB, frame.payload.data, frame.payload.len, compression_threshold) != 0) goto cleanup;
                    fprintf(stdout, "sent config known_packs len=%zu\n", frame.payload.len);
                } else if (frame.packet_id == PKT_CONFIG_KEEPALIVE_CB) {
                    if (frame.payload.len != 8) {
                        fprintf(stderr, "config keepalive len=%zu (expected 8)\n", frame.payload.len);
                        goto cleanup;
                    }
                    if (send_packet(&c, PKT_CONFIG_KEEPALIVE_SB, frame.payload.data, frame.payload.len, compression_threshold) != 0) goto cleanup;
                    fprintf(stdout, "sent config keepalive len=%zu\n", frame.payload.len);
                } else if (frame.packet_id == PKT_CONFIG_REGISTRY_DATA) {
                    if (write_varint_to_file(reg, (int32_t)frame.payload.len) != 0) goto cleanup;
                    if (fwrite(frame.payload.data, 1, frame.payload.len, reg) != frame.payload.len) goto cleanup;
                    registry_packets++;
                    fprintf(stdout, "registry packet %d len=%zu\n", registry_packets, frame.payload.len);
                } else if (frame.packet_id == PKT_CONFIG_UPDATE_TAGS) {
                    if (fwrite(frame.payload.data, 1, frame.payload.len, tags) != frame.payload.len) goto cleanup;
                    tags_written = true;
                    fprintf(stdout, "captured update tags len=%zu\n", frame.payload.len);
                } else if (frame.packet_id == 0x02) {
                    char reason[256];
                    if (read_string_payload(frame.payload.data, frame.payload.len, reason, sizeof(reason)) == 0) {
                        fprintf(stderr, "config disconnect: %s\n", reason);
                    } else {
                        fprintf(stderr, "config disconnect (unparsed)\n");
                    }
                    goto cleanup;
                } else if (frame.packet_id == PKT_CONFIG_FINISH_CB) {
                    if (send_packet(&c, PKT_CONFIG_FINISH_ACK, NULL, 0, compression_threshold) != 0) goto cleanup;
                    state = STATE_PLAY;
                    fprintf(stdout, "sent config finish ack\n");
                }
            } else if (state == STATE_PLAY) {
                if (frame.packet_id == PKT_PLAY_KEEPALIVE_CB) {
                    if (frame.payload.len != 8) {
                        fprintf(stderr, "play keepalive len=%zu (expected 8)\n", frame.payload.len);
                        goto cleanup;
                    }
                    if (send_packet(&c, PKT_PLAY_KEEPALIVE_SB, frame.payload.data, frame.payload.len, compression_threshold) != 0) goto cleanup;
                    fprintf(stdout, "sent play keepalive len=%zu\n", frame.payload.len);
                } else if (frame.packet_id == PKT_PLAY_SYNC_POS) {
                    if (handle_sync_pos(&c, &frame) != 0) goto cleanup;
                    fprintf(stdout, "sent confirm teleport\n");
                } else if (frame.packet_id == PKT_PLAY_CHUNK_DATA && !chunk_written) {
                    if (fwrite(frame.payload.data, 1, frame.payload.len, chunk) != frame.payload.len) goto cleanup;
                    chunk_written = true;
                    fprintf(stdout, "captured registry packets: %d\n", registry_packets);
                    fprintf(stdout, "captured first chunk\n");
                    goto cleanup;
                }
            }

            free(frame.payload.data);
        }
    }

cleanup:
    buf_free(&c.in);
    buf_free(&c.out);
    fclose(reg);
    fclose(tags);
    fclose(chunk);
    close(fd);
    if (registry_packets == 0 || !tags_written || !chunk_written) {
        fprintf(stderr, "no data captured. verify:\n");
        fprintf(stderr, " - target is a vanilla 26.1 server (not mc_server)\n");
        fprintf(stderr, " - online-mode=false\n");
        fprintf(stderr, " - network-compression-threshold=-1\n");
        fprintf(stderr, " - host/port are correct (try 127.0.0.1 if vanilla runs in WSL)\n");
    }
    return 0;
}
