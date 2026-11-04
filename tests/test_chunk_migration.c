#include "mc_chunk_migration.h"
#include "mc_anvil.h"
#include "block_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "migration check failed line %d: %s\n", __LINE__, #x);                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static mc_nbt_tag_t *get(mc_nbt_tag_t *root, const char *name) {
    return (mc_nbt_tag_t *)mc_nbt_compound_get(root, name);
}
static void result(const uint8_t *raw, size_t n, int32_t cx, int32_t cz, bool accept) {
    uint8_t *out = NULL;
    size_t out_n = 0;
    char reason[160];
    mc_chunk_migration_info_t info;
    int rc = mc_chunk_migrate_nbt(raw, n, cx, cz, &out, &out_n, &info, reason);
    CHECK((rc == 0) == accept);
    CHECK(*reason);
    if (!accept)
        CHECK(!out && !out_n);
    free(out);
}
static uint8_t *serialize(mc_nbt_tag_t *root, size_t *n) {
    uint8_t *raw = NULL;
    CHECK(mc_nbt_write_named_root(root, &raw, n) == 0);
    return raw;
}
static void palette(unsigned different, int version) {
    mc_chunk_t chunk = {0};
    CHECK(mc_chunk_init(&chunk, -33, 32, 0) == 0);
    unsigned at = 0;
    for (uint32_t id = 1; id < GLOBAL_BLOCK_STATES_COUNT && at < different; id++) {
        if (mc_native_state_sensitive(id))
            continue;
        CHECK(mc_chunk_set_block(&chunk, at & 15, 64 + (int)(at >> 8), (at >> 4) & 15, id) == 0);
        at++;
    }
    CHECK(at == different);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_anvil_encode_chunk_nbt(&chunk, NULL, &raw, &n) == 0);
    mc_nbt_tag_t *root = NULL;
    size_t used = 0;
    CHECK(mc_nbt_read_named_root(raw, n, &root, &used) == 0 && used == n);
    free(raw);
    get(root, "DataVersion")->payload.int_val = version;
    raw = serialize(root, &n);
    uint8_t *output = NULL;
    size_t output_n = 0;
    char reason[160];
    mc_chunk_migration_info_t info;
    CHECK(mc_chunk_migrate_nbt(raw, n, -33, 32, &output, &output_n, &info, reason) == 0);
    CHECK(info.source_version == version && info.compact_sections == 1);
    mc_chunk_t native = {0};
    CHECK(mc_anvil_decode_native(output, output_n, -33, 32, &native) == 0);
    for (int y = -64; y < 320; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                CHECK(mc_chunk_get_block(&native, x, y, z) == mc_chunk_get_block(&chunk, x, y, z));
    /* A padded source is accepted without changing its canonical NBT. */
    uint8_t *again = NULL;
    size_t again_n = 0;
    CHECK(mc_chunk_migrate_nbt(output, output_n, -33, 32, &again, &again_n, &info, reason) == 0);
    CHECK(output_n == again_n && !memcmp(output, again, again_n));
    free(again);
    result(raw, n, -32, 32, false);
    get(root, "DataVersion")->payload.int_val = 1;
    uint8_t *bad = serialize(root, &used);
    result(bad, used, -33, 32, false);
    free(bad);
    get(root, "DataVersion")->payload.int_val = version;
    mc_nbt_tag_t *sections = get(root, "sections");
    mc_nbt_tag_t *bs = get(sections->payload.list.items[8], "block_states"), *data = get(bs, "data");
    CHECK(data && data->payload.long_array.length > 0);
    int64_t saved = data->payload.long_array.data[0];
    data->payload.long_array.data[0] = INT64_MAX;
    bad = serialize(root, &used);
    result(bad, used, -33, 32, false);
    free(bad);
    data->payload.long_array.data[0] = saved;
    data->payload.long_array.length--;
    bad = serialize(root, &used);
    result(bad, used, -33, 32, false);
    free(bad);
    data->payload.long_array.length++;
    mc_nbt_tag_t *name = get(get(bs, "palette")->payload.list.items[1], "Name");
    char *old = name->payload.string_val;
    name->payload.string_val = "minecraft:does_not_exist";
    bad = serialize(root, &used);
    result(bad, used, -33, 32, false);
    free(bad);
    name->payload.string_val = old;
    uint8_t *trailing = malloc(n + 1);
    CHECK(trailing);
    memcpy(trailing, raw, n);
    trailing[n] = 0;
    result(trailing, n + 1, -33, 32, false);
    free(trailing);
    free(raw);
    free(output);
    mc_nbt_free(root);
    mc_chunk_destroy(&chunk);
    mc_chunk_destroy(&native);
}
int main(void) {
    for (unsigned version = 4785; version <= 4788; version += 3) {
        palette(16, (int)version);
        palette(32, (int)version);
        palette(256, (int)version);
    }
    printf("chunk migration: %u checks passed\n", checks);
    return 0;
}
