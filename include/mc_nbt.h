#ifndef MC_NBT_H
#define MC_NBT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"

typedef enum {
    MC_NBT_TAG_END = 0,
    MC_NBT_TAG_BYTE = 1,
    MC_NBT_TAG_SHORT = 2,
    MC_NBT_TAG_INT = 3,
    MC_NBT_TAG_LONG = 4,
    MC_NBT_TAG_FLOAT = 5,
    MC_NBT_TAG_DOUBLE = 6,
    MC_NBT_TAG_BYTE_ARRAY = 7,
    MC_NBT_TAG_STRING = 8,
    MC_NBT_TAG_LIST = 9,
    MC_NBT_TAG_COMPOUND = 10,
    MC_NBT_TAG_INT_ARRAY = 11,
    MC_NBT_TAG_LONG_ARRAY = 12,
} mc_nbt_type_t;

typedef struct mc_nbt_tag mc_nbt_tag_t;

struct mc_nbt_tag {
    mc_nbt_type_t type;
    char *name; /* NULL for list items and unnamed roots */

    union {
        int8_t byte_val;
        int16_t short_val;
        int32_t int_val;
        int64_t long_val;
        float float_val;
        double double_val;

        char *string_val;

        struct {
            int32_t length;
            int8_t *data;
        } byte_array;

        struct {
            int32_t length;
            int32_t *data;
        } int_array;

        struct {
            int32_t length;
            int64_t *data;
        } long_array;

        struct {
            mc_nbt_type_t elem_type;
            int32_t length;
            mc_nbt_tag_t **items;
        } list;

        struct {
            int32_t length;
            mc_nbt_tag_t **children;
        } compound;
    } payload;
};

/* Reads a named root tag (on-disk NBT): [type][name][payload]. */
int mc_nbt_read_named_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read);

/* Reads an unnamed root tag (network NBT): [type][payload]. */
int mc_nbt_read_unnamed_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read);

/* Reads a named root tag into an arena-backed tree. */
int mc_nbt_read_named_root_arena(const uint8_t *data, size_t len, mc_arena_t *arena, mc_nbt_tag_t **out, size_t *bytes_read);

/* Reads an unnamed root tag into an arena-backed tree. */
int mc_nbt_read_unnamed_root_arena(const uint8_t *data, size_t len, mc_arena_t *arena, mc_nbt_tag_t **out, size_t *bytes_read);

/* Writes a named root tag (on-disk NBT): [type][name][payload]. Output is malloc'ed. */
int mc_nbt_write_named_root(const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len);

/* Writes an unnamed root tag (network NBT): [type][payload]. Output is malloc'ed. */
int mc_nbt_write_unnamed_root(const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len);

void mc_nbt_free(mc_nbt_tag_t *tag);

const mc_nbt_tag_t *mc_nbt_compound_get(const mc_nbt_tag_t *compound, const char *name);

bool mc_anvil_validate_chunk(const mc_nbt_tag_t *root);

void mc_nbt_dump(const mc_nbt_tag_t *tag, FILE *out, int indent, int max_depth);

#endif /* MC_NBT_H */
