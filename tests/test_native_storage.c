/* S3A: native storage, preservation, conflicts and fail-closed selection. */
#include "mc_world_storage.h"
#include "mc_world.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_nbt.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d %s errno=%d\n", __FILE__, __LINE__, #x, errno);                           \
            abort();                                                                                         \
        }                                                                                                    \
    } while (0)
static int fail_dir_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    struct stat st;
    if (fail_dir_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        fail_dir_sync = 0;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
static char *temp(void) {
    char *p = strdup("/tmp/goatcore-native-s3a.XXXXXX");
    CHECK(p && mkdtemp(p));
    return p;
}
static mc_nbt_tag_t *tag(mc_nbt_type_t type, const char *name) {
    mc_nbt_tag_t *t = calloc(1, sizeof(*t));
    CHECK(t);
    t->type = type;
    t->name = name ? strdup(name) : NULL;
    return t;
}
static void add(mc_nbt_tag_t *root, mc_nbt_tag_t *t) {
    int n = root->payload.compound.length;
    void *p = realloc(root->payload.compound.children, (size_t)(n + 1) * sizeof(t));
    CHECK(p);
    root->payload.compound.children = p;
    root->payload.compound.children[n] = t;
    root->payload.compound.length++;
}
static mc_nbt_tag_t *get(mc_nbt_tag_t *root, const char *name) {
    return (mc_nbt_tag_t *)mc_nbt_compound_get(root, name);
}
static mc_nbt_tag_t *parse(const uint8_t *raw, size_t n) {
    mc_nbt_tag_t *r = NULL;
    size_t used = 0;
    CHECK(mc_nbt_read_named_root(raw, n, &r, &used) == 0 && used == n);
    return r;
}
static void serialize(mc_nbt_tag_t *r, uint8_t **raw, size_t *n) {
    CHECK(mc_nbt_write_named_root(r, raw, n) == 0);
}
static void path(char out[4096], const char *root, const char *suffix) {
    CHECK(snprintf(out, 4096, "%s/%s", root, suffix) < 4096);
}
static void marker(const char *root, const char *suffix) {
    char p[4096];
    path(p, root, suffix);
    int fd = open(p, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
}
static void preservation(void) {
    char *root = temp(), p[4096];
    mc_world_storage_t *store = NULL;
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &store) == 0);
    mc_chunk_t a, b;
    CHECK(mc_chunk_init(&a, -1, 32, 0) == 0);
    /* >16, >32 and >256 entries exercise padded 5/6/9-bit boundaries. */
    for (int layer = 0; layer < 3; layer++)
        for (unsigned i = 0, state = 1; i < (unsigned)(layer == 0   ? 20
                                                       : layer == 1 ? 40
                                                                    : 300);
             i++, state++) {
            while (mc_native_state_sensitive(state))
                state++;
            CHECK(mc_chunk_set_block(&a, i & 15, layer * 16 + (int)(i >> 8), (i >> 4) & 15, state) == 0);
        }
    CHECK(mc_storage_write(store, &a, NULL) == 0);
    CHECK(mc_storage_read(store, -1, 32, &b, NULL) == 0);
    for (int y = -64; y < 320; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                CHECK(mc_chunk_get_block(&a, x, y, z) == mc_chunk_get_block(&b, x, y, z));
    mc_nbt_tag_t *r = parse(a.storage_nbt, a.storage_nbt_len);
    mc_nbt_tag_t *opaque = tag(MC_NBT_TAG_STRING, "S3A:opaque");
    opaque->payload.string_val = strdup("preserve root, section, palette, biomes and tick data");
    add(r, opaque);
    mc_nbt_tag_t *secs = get(r, "sections"), *sec = secs->payload.list.items[4],
                 *bs = get(sec, "block_states");
    mc_nbt_tag_t *extra = tag(MC_NBT_TAG_LONG, "S3A:section");
    extra->payload.long_val = INT64_C(0x123456789abcdef);
    add(sec, extra);
    extra = tag(MC_NBT_TAG_INT, "S3A:palette");
    extra->payload.int_val = 123;
    add(get(bs, "palette")->payload.list.items[0], extra);
    extra = tag(MC_NBT_TAG_COMPOUND, "Heightmaps");
    add(r, extra);
    mc_nbt_tag_t *known = tag(MC_NBT_TAG_LONG_ARRAY, "WORLD_SURFACE");
    add(extra, known);
    known = tag(MC_NBT_TAG_LONG, "S3A:heightmap");
    known->payload.long_val = 77;
    add(extra, known);
    extra = tag(MC_NBT_TAG_BYTE_ARRAY, "SkyLight");
    add(sec, extra);
    uint8_t *raw = NULL;
    size_t n = 0;
    serialize(r, &raw, &n);
    mc_nbt_free(r);
    path(p, root, "dimensions/minecraft/overworld/region/r.-1.1.mca");
    CHECK(mc_anvil_write_chunk_nbt(p, 31, 0, raw, n) == 0);
    CHECK(mc_chunk_set_block(&b, 0, 64, 0, 1) == 0);
    CHECK(mc_storage_write(store, &b, NULL) == -1 &&
          errno == ESTALE); /* newer disk snapshot cannot be overwritten */
    mc_chunk_destroy(&b);
    CHECK(mc_storage_read(store, -1, 32, &b, NULL) == 0);
    CHECK(mc_chunk_set_block(&b, 0, 64, 0, 1) == 0);
    fail_dir_sync = 1;
    CHECK(mc_storage_write(store, &b, NULL) == MC_SAVE_DURABILITY_UNCERTAIN);
    CHECK(b.storage_nbt &&
          mc_storage_write(store, &b, NULL) == 0); /* visible snapshot advanced, retry safe */
    r = parse(b.storage_nbt, b.storage_nbt_len);
    secs = get(r, "sections");
    sec = secs->payload.list.items[4];
    CHECK(!strcmp(get(r, "S3A:opaque")->payload.string_val,
                  "preserve root, section, palette, biomes and tick data"));
    CHECK(get(sec, "S3A:section")->payload.long_val == INT64_C(0x123456789abcdef));
    CHECK(get(get(get(sec, "block_states"), "palette")->payload.list.items[0], "S3A:palette")
              ->payload.int_val == 123);
    CHECK(!get(get(r, "Heightmaps"), "WORLD_SURFACE") &&
          get(get(r, "Heightmaps"), "S3A:heightmap")->payload.long_val == 77);
    CHECK(!get(sec, "SkyLight") && get(r, "isLightOn")->payload.byte_val == 0);
    mc_nbt_free(r);
    /* Updating a second chunk preserves the exact first chunk NBT. */
    mc_chunk_t c;
    CHECK(mc_chunk_init(&c, -2, 32, 0) == 0);
    CHECK(mc_storage_write(store, &c, NULL) == 0);
    uint8_t *again = NULL;
    size_t an = 0;
    CHECK(mc_anvil_read_chunk_nbt(p, 31, 0, &again, &an) == 0);
    CHECK(an == b.storage_nbt_len && !memcmp(again, b.storage_nbt, an));
    free(again);
    /* An unsupported neighbor codec forbids replacement of the whole region. */
    int fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    uint8_t entry[4];
    CHECK(mc_file_pread_all(fd, entry, 4, 30 * 4) == 0);
    unsigned off = ((unsigned)entry[0] << 16) | ((unsigned)entry[1] << 8) | entry[2];
    uint8_t codec = 4;
    CHECK(mc_file_pwrite_all(fd, &codec, 1, (off_t)off * 4096 + 4) == 0 && close(fd) == 0);
    CHECK(mc_storage_write(store, &b, NULL) == -1 && errno == EOPNOTSUPP);
    CHECK(mc_storage_read(store, -1, 32, &c, NULL) == -1); /* no fallback or regeneration */
    mc_chunk_destroy(&a);
    mc_chunk_destroy(&b);
    mc_chunk_destroy(&c);
    free(raw);
    mc_storage_close(store);
    free(root);
}
static void invalid_chunks(void) {
    mc_chunk_t c, out;
    CHECK(mc_chunk_init(&c, 0, 0, 0) == 0);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_anvil_encode_native(&c, NULL, &raw, &n) == 0);
    mc_nbt_tag_t *r = parse(raw, n);
    uint8_t *bad = NULL;
    size_t bn = 0;
    get(r, "DataVersion")->payload.int_val = 4785;
    serialize(r, &bad, &bn);
    CHECK(mc_anvil_decode_native(bad, bn, 0, 0, &out) == -1);
    free(bad);
    get(r, "DataVersion")->payload.int_val = 4788;
    get(r, "xPos")->payload.int_val = 1;
    serialize(r, &bad, &bn);
    CHECK(mc_anvil_decode_native(bad, bn, 0, 0, &out) == -1);
    free(bad);
    get(r, "xPos")->payload.int_val = 0;
    mc_nbt_tag_t *name = get(
        get(get(get(r, "sections")->payload.list.items[0], "block_states"), "palette")->payload.list.items[0],
        "Name");
    free(name->payload.string_val);
    name->payload.string_val = strdup("minecraft:not_a_block");
    serialize(r, &bad, &bn);
    CHECK(mc_anvil_decode_native(bad, bn, 0, 0, &out) == -1 && errno == EOPNOTSUPP);
    free(bad);
    mc_nbt_free(r);
    r = parse(raw, n);
    mc_nbt_tag_t *be = get(r, "block_entities");
    be->payload.list.items = calloc(1, sizeof(void *));
    CHECK(be->payload.list.items);
    be->payload.list.items[0] = tag(MC_NBT_TAG_COMPOUND, NULL);
    be->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    be->payload.list.length = 1;
    serialize(r, &bad, &bn);
    CHECK(mc_anvil_decode_native(bad, bn, 0, 0, &out) == -1);
    free(bad);
    mc_nbt_free(r);
    /* An implicit air section can be materialized without discarding metadata. */
    r = parse(raw, n);
    mc_nbt_tag_t *sections = get(r, "sections");
    mc_nbt_free(sections->payload.list.items[5]);
    memmove(sections->payload.list.items + 5, sections->payload.list.items + 6,
            (size_t)(sections->payload.list.length - 6) * sizeof(void *));
    sections->payload.list.length--;
    serialize(r, &bad, &bn);
    CHECK(mc_anvil_decode_native(bad, bn, 0, 0, &out) == 0);
    out.storage_nbt = bad;
    out.storage_nbt_len = bn;
    CHECK(mc_chunk_set_block(&out, 0, 16, 0, 1) == 0);
    uint8_t *materialized = NULL;
    size_t materialized_length = 0;
    CHECK(mc_anvil_encode_native(&out, NULL, &materialized, &materialized_length) == 0);
    mc_chunk_t restored;
    CHECK(mc_anvil_decode_native(materialized, materialized_length, 0, 0, &restored) == 0);
    CHECK(mc_chunk_get_block(&restored, 0, 16, 0) == 1);
    free(materialized);
    mc_chunk_destroy(&restored);
    mc_chunk_destroy(&out);
    mc_nbt_free(r);
    CHECK(mc_anvil_decode_native(raw, n - 1, 0, 0, &out) == -1);
    mc_chunk_destroy(&c);
    free(raw);
}
static void selection(void) {
    char *root = temp();
    mc_world_storage_t *a = NULL, *b = NULL;
    CHECK(mc_storage_parse("anvil-atomic-experimental", &(mc_storage_kind_t){0}) == 0);
    CHECK(mc_storage_parse("anvil", &(mc_storage_kind_t){0}) == -1);
    CHECK(mc_storage_open(root, MC_STORAGE_BIN, &a) == 0);
    mc_chunk_t c;
    CHECK(mc_chunk_init(&c, 0, 0, 0) == 0);
    CHECK(mc_storage_write(a, &c, NULL) == 0);
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &b) == -1);
    mc_storage_close(a);
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &b) == -1);
    mc_chunk_destroy(&c);
    free(root);
    root = temp();
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &a) == 0);
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &b) == -1);
    CHECK(mc_storage_open(root, MC_STORAGE_BIN, &b) == -1);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        mc_world_storage_t *child = NULL;
        _exit(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &child) == -1 ? 0 : 1);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    mc_storage_close(a);
    CHECK(mc_storage_open(root, MC_STORAGE_BIN, &b) == -1);
    marker(root, "dimensions/minecraft/overworld/region/r.0.0.mca.s2-journal");
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &a) == -1 && errno == EBUSY);
    free(root);
}
static void vanilla_session_lock(void) {
    char *root = temp(), p[4096];
    path(p, root, "session.lock");
    int ready[2], release[2];
    CHECK(pipe(ready) == 0 && pipe(release) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(ready[0]);
        close(release[1]);
        int fd = open(p, O_CREAT | O_RDWR, 0600);
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 0};
        if (fd < 0 || fcntl(fd, F_SETLK, &lock) != 0)
            _exit(2);
        if (write(ready[1], "x", 1) != 1)
            _exit(3);
        char byte;
        if (read(release[0], &byte, 1) != 1)
            _exit(4);
        close(fd);
        _exit(0);
    }
    close(ready[1]);
    close(release[0]);
    char byte;
    CHECK(read(ready[0], &byte, 1) == 1);
    mc_world_storage_t *s = NULL;
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &s) == -1);
    CHECK(write(release[1], "x", 1) == 1);
    close(ready[0]);
    close(release[1]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &s) == 0);
    mc_storage_close(s);
    free(root);
}
static void world_roundtrip(void) {
    char *root = temp();
    const int pos[][2] = {{-513, -1}, {-512, 0}, {-17, 15}, {-16, 16}, {511, 511}, {512, 512}};
    for (int pass = 0; pass < 3; pass++) {
        mc_world_t *w = mc_world_create_with_storage(root, 0, 4, MC_STORAGE_ANVIL_ATOMIC);
        CHECK(w);
        for (unsigned i = 0; i < sizeof(pos) / sizeof(pos[0]); i++) {
            int32_t state = -1;
            int ready = 1;
            for (int retry = 0; retry < 3000 && ready == 1; retry++) {
                mc_world_tick(w, 0);
                ready = mc_world_get_block_ready(w, pos[i][0], 70, pos[i][1], &state);
                struct timespec ts = {0, 1000000};
                if (ready == 1)
                    nanosleep(&ts, NULL);
            }
            CHECK(ready == 0);
            if (pass)
                CHECK(state == (int32_t)(i + 1));
            bool allowed = false;
            for (int retry = 0; retry < 3000 && !allowed; retry++) {
                mc_world_tick(w, 0);
                allowed = mc_world_storage_action_allowed(w, pos[i][0], 70, pos[i][1]);
                if (!allowed) {
                    struct timespec ts = {0, 1000000};
                    nanosleep(&ts, NULL);
                }
            }
            CHECK(allowed);
            CHECK(mc_world_set_block(w, pos[i][0], 70, pos[i][1], i + 1) == 0);
            CHECK(mc_world_flush_block(w, pos[i][0], 70, pos[i][1]) == 0);
        }
        CHECK(mc_world_evict_outside(w, NULL, 0, 100) > 0);
        int32_t state;
        for (int retry = 0; retry < 3000; retry++) {
            mc_world_tick(w, 0);
            if (!mc_world_get_block_ready(w, -513, 70, -1, &state))
                break;
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        CHECK(state == 1);
        mc_world_destroy(w);
    }
    char p[4096];
    path(p, root, "chunks");
    CHECK(access(p, F_OK) == -1 && errno == ENOENT);
    free(root);
}
#include "generated_native_be_blocks.h"
static void opaque_entities(void) {
    CHECK(native_sensitive_by_block[0] == 0);
    uint32_t chest =
        mc_world_runtime_state_id_from_key("minecraft:chest[facing=north,type=single,waterlogged=false]", -1);
    mc_chunk_t source, loaded;
    CHECK(mc_chunk_init(&source, 0, 0, 0) == 0);
    CHECK(mc_chunk_set_block(&source, 2, 64, 2, chest) == 0);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_anvil_encode_chunk_nbt(&source, NULL, &raw, &n) == 0);
    mc_nbt_tag_t *root = parse(raw, n);
    free(raw);
    mc_nbt_tag_t *list = get(root, "block_entities");
    list->payload.list.items = calloc(2, sizeof(void *));
    CHECK(list->payload.list.items);
    mc_nbt_tag_t *be = tag(MC_NBT_TAG_COMPOUND, NULL);
    list->payload.list.items[0] = be;
    list->payload.list.length = 1;
    mc_nbt_tag_t *id = tag(MC_NBT_TAG_STRING, "id");
    id->payload.string_val = strdup("minecraft:chest");
    add(be, id);
    for (unsigned i = 0; i < 3; i++) {
        mc_nbt_tag_t *t = tag(MC_NBT_TAG_INT, (const char *[]){"x", "y", "z"}[i]);
        t->payload.int_val = i == 1 ? 64 : 2;
        add(be, t);
    }
    mc_nbt_tag_t *custom = tag(MC_NBT_TAG_COMPOUND, "S3B:opaque");
    add(be, custom);
    mc_nbt_tag_t *blob = tag(MC_NBT_TAG_BYTE_ARRAY, "bytes");
    blob->payload.byte_array.length = 4;
    blob->payload.byte_array.data = malloc(4);
    CHECK(blob->payload.byte_array.data);
    memcpy(blob->payload.byte_array.data, "a\0\xffz", 4);
    add(custom, blob);
    mc_nbt_tag_t *items = tag(MC_NBT_TAG_LIST, "Items");
    items->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    add(be, items);
    uint8_t *be_before = NULL;
    size_t be_n = 0;
    serialize(be, &be_before, &be_n);
    serialize(root, &raw, &n);
    CHECK(mc_anvil_decode_native(raw, n, 0, 0, &loaded) == 0);
    loaded.storage_nbt = raw;
    loaded.storage_nbt_len = n;
    CHECK(loaded.storage_guard && !loaded.storage_readonly);
    CHECK(mc_chunk_set_block(&loaded, 10, 64, 10, 1) == 0);
    uint8_t *saved = NULL;
    size_t sn = 0;
    CHECK(mc_anvil_encode_native(&loaded, NULL, &saved, &sn) == 0);
    mc_nbt_tag_t *after = parse(saved, sn);
    uint8_t *be_after = NULL;
    size_t an = 0;
    serialize(get(after, "block_entities")->payload.list.items[0], &be_after, &an);
    CHECK(an == be_n && !memcmp(be_before, be_after, an));
    free(be_before);
    free(be_after);
    free(saved);
    mc_nbt_free(after);
    char *world_root = temp(), region[4096];
    mc_world_storage_t *store = NULL;
    CHECK(mc_storage_open(world_root, MC_STORAGE_ANVIL_ATOMIC, &store) == 0);
    path(region, world_root, "dimensions/minecraft/overworld/region/r.0.0.mca");
    CHECK(mc_anvil_write_chunk_nbt(region, 0, 0, loaded.storage_nbt, loaded.storage_nbt_len) == 0);
    CHECK(mc_storage_write(store, &loaded, NULL) == 0);
    CHECK(mc_chunk_set_block(&loaded, 11, 64, 10, 1) == 0);
    fail_dir_sync = 1;
    CHECK(mc_storage_write(store, &loaded, NULL) == MC_SAVE_DURABILITY_UNCERTAIN);
    CHECK(mc_storage_write(store, &loaded, NULL) == 0);
    mc_storage_close(store);
    free(world_root);
    CHECK(mc_chunk_set_block(&loaded, 2, 64, 2, 0) == 0);
    CHECK(mc_anvil_encode_native(&loaded, NULL, &saved, &sn) == -1);
    CHECK(mc_chunk_set_block(&loaded, 2, 64, 2, chest) == 0);
    CHECK(mc_chunk_set_block(&loaded, 2, 63, 2, 1) == 0);
    CHECK(mc_anvil_encode_native(&loaded, NULL, &saved, &sn) == -1);
    mc_chunk_destroy(&loaded);
    /* Every official isValid pair, not a hand-maintained family heuristic. */
    for (size_t k = 0; k < sizeof(native_be_blocks) / sizeof(*native_be_blocks); k++) {
        uint32_t state = UINT32_MAX;
        for (size_t j = 0; j < GLOBAL_BLOCK_COUNT; j++)
            if (!strcmp(GLOBAL_BLOCKS[j].name, native_be_blocks[k].block))
                state = GLOBAL_BLOCKS[j].default_state;
        CHECK(state != UINT32_MAX && mc_native_state_sensitive(state));
        CHECK(mc_chunk_set_block(&source, 2, 64, 2, state) == 0);
        uint8_t *base = NULL;
        size_t bn = 0;
        CHECK(mc_anvil_encode_chunk_nbt(&source, NULL, &base, &bn) == 0);
        mc_nbt_tag_t *r = parse(base, bn);
        free(base);
        mc_nbt_tag_t *bl = get(r, "block_entities");
        uint8_t *entry = NULL;
        size_t en = 0;
        serialize(be, &entry, &en);
        mc_nbt_tag_t *copy = parse(entry, en);
        free(entry);
        mc_nbt_tag_t *name = get(copy, "id");
        free(name->payload.string_val);
        name->payload.string_val = strdup(native_be_blocks[k].entity);
        mc_nbt_tag_t *timer = NULL;
        if (!strcmp(native_be_blocks[k].entity, "minecraft:furnace")) {
            timer = tag(MC_NBT_TAG_SHORT, "lit_time_remaining");
            add(copy, timer);
        }
        bl->payload.list.items = calloc(1, sizeof(void *));
        CHECK(bl->payload.list.items);
        bl->payload.list.items[0] = copy;
        bl->payload.list.length = 1;
        serialize(r, &base, &bn);
        CHECK(mc_anvil_decode_native(base, bn, 0, 0, &loaded) == 0);
        if (!strcmp(native_be_blocks[k].entity, "minecraft:piston"))
            CHECK(loaded.storage_readonly);
        mc_chunk_destroy(&loaded);
        if (timer) {
            timer->payload.short_val = 100;
            uint8_t *active = NULL;
            size_t active_n = 0;
            serialize(r, &active, &active_n);
            CHECK(mc_anvil_decode_native(active, active_n, 0, 0, &loaded) == 0 && loaded.storage_readonly);
            mc_chunk_destroy(&loaded);
            free(active);
        }
        free(base);
        mc_nbt_free(r);
    }
    items->type = MC_NBT_TAG_INT;
    serialize(root, &raw, &n);
    CHECK(mc_anvil_decode_native(raw, n, 0, 0, &loaded) == -1);
    free(raw);
    items->type = MC_NBT_TAG_LIST;
    const char *bad_ids[] = {"minecraft:unknown_s3b", "minecraft:furnace"};
    for (unsigned i = 0; i < 2; i++) {
        free(id->payload.string_val);
        id->payload.string_val = strdup(bad_ids[i]);
        serialize(root, &raw, &n);
        CHECK(mc_anvil_decode_native(raw, n, 0, 0, &loaded) == -1);
        free(raw);
    }
    free(id->payload.string_val);
    id->payload.string_val = strdup("minecraft:chest");
    get(be, "x")->payload.int_val = 16;
    serialize(root, &raw, &n);
    CHECK(mc_anvil_decode_native(raw, n, 0, 0, &loaded) == -1);
    free(raw);
    get(be, "x")->payload.int_val = 2;
    mc_nbt_tag_t *ticks = tag(MC_NBT_TAG_LIST, "block_ticks");
    ticks->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    ticks->payload.list.length = 1;
    ticks->payload.list.items = calloc(1, sizeof(void *));
    CHECK(ticks->payload.list.items);
    ticks->payload.list.items[0] = tag(MC_NBT_TAG_COMPOUND, NULL);
    add(root, ticks);
    serialize(root, &raw, &n);
    CHECK(mc_anvil_decode_native(raw, n, 0, 0, &loaded) == 0 && loaded.storage_readonly);
    loaded.storage_nbt = raw;
    loaded.storage_nbt_len = n;
    CHECK(mc_chunk_set_block(&loaded, 10, 64, 10, 1) == 0);
    CHECK(mc_anvil_encode_native(&loaded, NULL, &saved, &sn) == -1);
    mc_chunk_destroy(&loaded);
    mc_nbt_free(root);
    mc_chunk_destroy(&source);
}
int main(void) {
    opaque_entities();
    selection();
    vanilla_session_lock();
    invalid_chunks();
    preservation();
    world_roundtrip();
    printf("test_native_storage: %u checks passed\n", checks);
    return 0;
}
