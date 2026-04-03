#include "block_registry.h"
#include "paletted_container.h"

#include <assert.h>
#include <stdio.h>

static void test_uniform_air(void) {
    mc_paletted_container_t sec;
    int rc = mc_paletted_container_init(&sec, 0u);
    assert(rc == 0);
    assert(mc_paletted_container_bits_per_block(&sec) == 0u);
    assert(mc_paletted_container_palette_len(&sec) == 0u);
    assert(mc_paletted_container_non_air_count(&sec) == 0u);
    assert(mc_paletted_container_get_block(&sec, 0, 0, 0) == 0u);
    assert(mc_paletted_container_get_block(&sec, 15, 15, 15) == 0u);
    assert(mc_paletted_container_get_block(&sec, -1, 0, 0) == 0u);
    mc_paletted_container_destroy(&sec);
}

static void test_first_set_promotes_to_palette4(void) {
    mc_paletted_container_t sec;
    int rc = mc_paletted_container_init(&sec, 0u);
    assert(rc == 0);
    assert(mc_paletted_container_set_block(&sec, 1, 2, 3, 1u) == 0);
    assert(mc_paletted_container_bits_per_block(&sec) == 4u);
    assert(!mc_paletted_container_is_direct(&sec));
    assert(mc_paletted_container_palette_len(&sec) == 2u);
    assert(sec.words != NULL);
    assert(sec.word_count == 256u);
    assert(mc_paletted_container_get_block(&sec, 1, 2, 3) == 1u);
    assert(mc_paletted_container_get_block(&sec, 0, 0, 0) == 0u);
    assert(mc_paletted_container_non_air_count(&sec) == 1u);
    mc_paletted_container_destroy(&sec);
}

static void test_coordinate_roundtrip(void) {
    mc_paletted_container_t sec;
    int rc = mc_paletted_container_init(&sec, 0u);
    assert(rc == 0);
    assert(mc_paletted_container_set_block(&sec, 3, 12, 8, 6u) == 0);
    assert(mc_paletted_container_get_block(&sec, 3, 12, 8) == 6u);
    assert(mc_paletted_container_get_block(&sec, 3, 12, 7) == 0u);
    mc_paletted_container_destroy(&sec);
}

static void test_resize_to_direct15(void) {
    mc_paletted_container_t sec;
    mc_global_state_id_t ids[16] = {
        1u, 2u, 3u, 4u, 5u, 6u,
        100u, 101u, 102u, 103u, 104u, 105u, 106u, 107u, 108u, 109u
    };
    int i;

    assert(mc_paletted_container_init(&sec, 0u) == 0);
    for (i = 0; i < 15; i++) {
        assert(mc_paletted_container_set_block(&sec, i & 15, 0, 0, ids[i]) == 0);
    }
    assert(mc_paletted_container_bits_per_block(&sec) == 4u);
    assert(mc_paletted_container_palette_len(&sec) == 16u);
    assert(!mc_paletted_container_is_direct(&sec));

    assert(mc_paletted_container_set_block(&sec, 15, 0, 0, ids[15]) == 0);
    assert(mc_paletted_container_bits_per_block(&sec) == 15u);
    assert(mc_paletted_container_is_direct(&sec));
    assert(mc_paletted_container_palette_len(&sec) == 0u);
    assert(sec.word_count == 960u);

    for (i = 0; i < 16; i++) {
        assert(mc_paletted_container_get_block(&sec, i & 15, 0, 0) == ids[i]);
    }
    mc_paletted_container_destroy(&sec);
}

static void test_non_air_counter_and_idempotence(void) {
    mc_paletted_container_t sec;
    assert(mc_paletted_container_init(&sec, 0u) == 0);

    assert(mc_paletted_container_set_block(&sec, 0, 0, 0, 1u) == 0);
    assert(mc_paletted_container_non_air_count(&sec) == 1u);
    assert(mc_paletted_container_set_block(&sec, 0, 0, 0, 2u) == 0);
    assert(mc_paletted_container_non_air_count(&sec) == 1u);
    assert(mc_paletted_container_set_block(&sec, 0, 0, 0, 2u) == 0);
    assert(mc_paletted_container_non_air_count(&sec) == 1u);
    assert(mc_paletted_container_set_block(&sec, 0, 0, 0, 0u) == 0);
    assert(mc_paletted_container_non_air_count(&sec) == 0u);

    mc_paletted_container_destroy(&sec);
}

int main(void) {
    test_uniform_air();
    test_first_set_promotes_to_palette4();
    test_coordinate_roundtrip();
    test_resize_to_direct15();
    test_non_air_counter_and_idempotence();
    puts("test_paletted_container: ok");
    return 0;
}
