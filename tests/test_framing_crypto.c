#include "mc_protocol.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const uint8_t secret[16] = "stream-secret!!";
    mc_conn_t sender = {0}, receiver = {0};
    assert(pthread_mutex_init(&sender.out_lock, NULL) == 0);
    assert(buf_init(&sender.out, 128) == 0);
    assert(buf_init(&receiver.in, 128) == 0);
    sender.out.limit = MC_MAX_OUTPUT_BUFFER;
    receiver.in.limit = MC_MAX_INPUT_BUFFER;
    atomic_init(&sender.crypto_phase, 0);
    atomic_init(&receiver.crypto_phase, 0);
    assert(mc_crypto_enable(&sender, secret) == 0);
    assert(mc_crypto_enable(&receiver, secret) == 0);
    atomic_store(&sender.crypto_phase, 2);
    atomic_store(&receiver.crypto_phase, 2);
    assert(conn_write_packet(&sender, 2, (const uint8_t *)"alpha", 5, -1) == 0);
    assert(conn_write_packet(&sender, 3, (const uint8_t *)"beta", 4, -1) == 0);
    assert(sender.out.len == 13);
    for (size_t i = 0; i < sender.out.len; i++) {
        uint8_t byte = sender.out.data[i];
        assert(mc_crypto_read(&receiver, &byte, 1) == 0);
        assert(buf_write(&receiver.in, &byte, 1) == 0);
        mc_frame_t frame = {0};
        int rc = conn_read_frame(&receiver, &frame, -1);
        if (i < 7) {
            if (i == 6) {
                assert(rc == 0 && frame.packet_id == 2 && frame.payload.len == 5);
                assert(memcmp(frame.payload.data, "alpha", 5) == 0);
            } else assert(rc == 1);
        } else if (i == 12) {
            assert(rc == 0 && frame.packet_id == 3 && frame.payload.len == 4);
            assert(memcmp(frame.payload.data, "beta", 4) == 0);
        } else assert(rc == 1);
        free(frame.payload.data);
    }
    mc_crypto_free(&sender);
    mc_crypto_free(&receiver);
    buf_free(&sender.out);
    buf_free(&receiver.in);
    pthread_mutex_destroy(&sender.out_lock);
    puts("framing crypto: encrypted lengths, continuous CFB8 and one-byte receives ok");
    return 0;
}
