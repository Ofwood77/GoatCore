#include "mc_anvil.h"
#include "mc_nbt.h"
#include "mc_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s <region.mca> <local_x 0..31> <local_z 0..31> [--tree] [--max-depth N]\n", argv0);
}

static bool parse_i32(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    if (v < -2147483648L || v > 2147483647L) return false;
    *out = (int)v;
    return true;
}

static const mc_nbt_tag_t *find_int(const mc_nbt_tag_t *compound, const char *name) {
    const mc_nbt_tag_t *t = mc_nbt_compound_get(compound, name);
    if (t && t->type == MC_NBT_TAG_INT) return t;
    return NULL;
}

static void print_chunk_coords(const mc_nbt_tag_t *root) {
    const mc_nbt_tag_t *x = find_int(root, "xPos");
    const mc_nbt_tag_t *z = find_int(root, "zPos");

    if (!x || !z) {
        const mc_nbt_tag_t *level = mc_nbt_compound_get(root, "Level");
        if (level && level->type == MC_NBT_TAG_COMPOUND) {
            if (!x) x = find_int(level, "xPos");
            if (!z) z = find_int(level, "zPos");
        }
    }

    if (x && z) {
        log_info("chunk coords: xPos=%d zPos=%d", x->payload.int_val, z->payload.int_val);
    } else {
        log_error("could not find xPos/zPos in parsed NBT");
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int local_x = 0;
    int local_z = 0;
    if (!parse_i32(argv[2], &local_x) || !parse_i32(argv[3], &local_z)) {
        usage(argv[0]);
        return 1;
    }

    bool dump_tree = false;
    int max_depth = 6;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0) {
            dump_tree = true;
            continue;
        }
        if (strcmp(argv[i], "--max-depth") == 0 && i + 1 < argc) {
            if (!parse_i32(argv[i + 1], &max_depth)) {
                usage(argv[0]);
                return 1;
            }
            i++;
            continue;
        }
        usage(argv[0]);
        return 1;
    }

    uint8_t *nbt = NULL;
    size_t nbt_len = 0;
    int rc = mc_anvil_read_chunk_nbt(path, local_x, local_z, &nbt, &nbt_len);
    if (rc == 1) {
        log_error("chunk %d,%d not present in region file", local_x, local_z);
        return 2;
    }
    if (rc != 0) {
        log_error("failed to read chunk NBT from region file");
        return 2;
    }

    mc_nbt_tag_t *root = NULL;
    size_t consumed = 0;
    if (mc_nbt_read_named_root(nbt, nbt_len, &root, &consumed) != 0) {
        free(nbt);
        log_error("NBT parse failed (len=%zu)", nbt_len);
        return 3;
    }
    free(nbt);

    if (root->type != MC_NBT_TAG_COMPOUND) {
        mc_nbt_free(root);
        log_error("root is not a compound (type=%d)", (int)root->type);
        return 3;
    }

    print_chunk_coords(root);

    if (dump_tree) {
        mc_nbt_dump(root, stdout, 0, max_depth);
    }

    mc_nbt_free(root);
    (void)consumed;
    return 0;
}
