#include "mc_region_storage.h"
#include "mc_atomic_file.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s errno=%d\n", __FILE__, __LINE__, #x, errno);                          \
            abort();                                                                                         \
        }                                                                                                    \
    } while (0)
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static void slot(int fd, unsigned i, unsigned off, unsigned n, unsigned len, unsigned codec) {
    uint8_t entry[4], record[5];
    put32(entry, off << 8 | n);
    CHECK(mc_file_pwrite_all(fd, entry, 4, i * 4) == 0);
    put32(entry, 123456 + i);
    CHECK(mc_file_pwrite_all(fd, entry, 4, 4096 + i * 4) == 0);
    if (off >= 2 && n) {
        put32(record, len);
        record[4] = codec;
        CHECK(mc_file_pwrite_all(fd, record, 5, (off_t)off * 4096) == 0);
    }
}
static void reset(int fd, unsigned sectors) {
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, (off_t)sectors * 4096) == 0);
}
static mc_region_index_t *load(int fd) {
    mc_region_index_t *v = NULL;
    CHECK(mc_region_index_load(fd, &v) == 0);
    CHECK(v != NULL);
    return v;
}
static void bad(int fd) {
    mc_region_index_t *v = NULL;
    CHECK(mc_region_index_load(fd, &v) == -1);
    CHECK(v == NULL);
}
int main(void) {
    char path[] = "/tmp/goatcore-region-s1.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    reset(fd, 2);
    mc_region_index_t *v = load(fd);
    CHECK(mc_region_stats(v)->chunks == 0 && mc_region_stats(v)->occupied == 2);
    CHECK(mc_region_sector_used(v, 0) && mc_region_sector_used(v, 1));
    CHECK(!mc_region_sector_used(v, 2));
    mc_region_index_free(v);
    reset(fd, 10);
    slot(fd, 0, 2, 2, 3, 2);
    slot(fd, 1023, 7, 1, 1, 3);
    v = load(fd);
    const mc_region_stats_t *st = mc_region_stats(v);
    CHECK(st->chunks == 2 && st->occupied == 5 && st->free_sectors == 5);
    for (unsigned s = 0; s < 10; s++)
        CHECK(mc_region_sector_used(v, s) == (s < 4 || s == 7));
    CHECK(mc_region_entry(v, 1023)->timestamp == 124479);
    CHECK(mc_region_entry(v, 1024) == NULL);
    mc_region_plan_t *p = NULL;
    CHECK(mc_region_plan_create(v, &p) == 0);
    CHECK(mc_region_plan_source(p)->inode == mc_region_source(v)->inode);
    CHECK(mc_region_plan_protect(p, 4, 1) == 0);
    CHECK(mc_region_plan_retire(p, 0, 42) == 0);
    CHECK(mc_region_plan_retire(p, 0, 43) == -1 && errno == EALREADY);
    CHECK(mc_region_plan_retirement(p, 0)->epoch == 42);
    CHECK(mc_region_plan_used(p, 2) && mc_region_plan_used(p, 3));
    mc_region_reservation_t a, b;
    CHECK(mc_region_plan_reserve(p, 2, &a) == 0 && a.offset == 5);
    CHECK(mc_region_plan_protect(p, 8, 1) == -1 && errno == EBUSY);
    CHECK(mc_region_plan_reserve(p, 3, &b) == 0 && b.offset == 8);
    CHECK(mc_region_plan_sectors(p) == 11);
    CHECK(mc_region_plan_reserve_at(p, 2, 1, &(mc_region_reservation_t){0}) == -1 && errno == EEXIST);
    CHECK(mc_region_plan_cancel(p, a) == 0);
    CHECK(mc_region_plan_cancel(p, a) == -1);
    CHECK(mc_region_plan_reserve(p, 2, &a) == 0 && a.offset == 5);
    mc_region_reservation_t old = a;
    CHECK(mc_region_plan_cancel(p, a) == 0);
    CHECK(mc_region_plan_reserve(p, 2, &a) == 0 && a.ticket != old.ticket);
    CHECK(mc_region_plan_cancel(p, old) == -1 && mc_region_plan_used(p, 5));
    mc_region_plan_t *other = NULL;
    CHECK(mc_region_plan_create(v, &other) == 0);
    CHECK(mc_region_plan_cancel(other, a) == -1);
    CHECK(mc_region_plan_used(p, 4)); /* journal pin never reclaimed */
    mc_region_plan_free(other);
    mc_region_plan_free(p);
    mc_region_index_free(v);
    CHECK(mc_region_plan_create(NULL, &p) == 0);
    CHECK(mc_region_plan_reserve_at(p, MC_REGION_MAX_OFFSET, 255, &a) == 0);
    CHECK(mc_region_plan_sectors(p) == MC_REGION_MAX_SECTORS);
    CHECK(mc_region_plan_used(p, MC_REGION_MAX_SECTORS - 1));
    CHECK(mc_region_plan_reserve_at(p, MC_REGION_MAX_OFFSET + 1, 1, &b) == -1 && errno == EFBIG);
    CHECK(mc_region_plan_reserve(p, 0, &b) == -1);
    CHECK(mc_region_plan_reserve(p, 256, &b) == -1);
    CHECK(mc_region_plan_cancel(p, a) == 0);
    mc_region_plan_free(p);
    /* All 1024 locations populated, exact accounting and no free payload sector. */
    reset(fd, 1026);
    for (unsigned i = 0; i < 1024; i++)
        slot(fd, i, 2 + i, 1, 1, 3);
    v = load(fd);
    CHECK(mc_region_stats(v)->chunks == 1024 && mc_region_stats(v)->free_sectors == 0);
    CHECK(mc_region_plan_create(v, &p) == 0);
    CHECK(mc_region_plan_reserve(p, 1, &a) == 0 && a.offset == 1026);
    mc_region_plan_free(p);
    mc_region_index_free(v);
    /* Malformations are not repaired, ignored or silently rewritten. */
    reset(fd, 3);
    slot(fd, 0, 1, 1, 1, 3);
    bad(fd);
    reset(fd, 3);
    slot(fd, 0, 2, 0, 1, 3);
    bad(fd);
    reset(fd, 3);
    slot(fd, 0, 3, 1, 1, 3);
    bad(fd);
    reset(fd, 4);
    slot(fd, 0, 2, 2, 1, 3);
    slot(fd, 1, 3, 1, 1, 3);
    bad(fd);
    reset(fd, 3);
    slot(fd, 0, 2, 1, 4093, 3);
    bad(fd);
    reset(fd, 3);
    slot(fd, 0, 2, 1, UINT32_MAX, 3);
    bad(fd);
    reset(fd, 3);
    slot(fd, 0, 2, 1, 0, 3);
    bad(fd);
    reset(fd, 3);
    CHECK(ftruncate(fd, 8193) == 0);
    bad(fd);
    reset(fd, 1);
    bad(fd);
    reset(fd, 0);
    bad(fd);
    /* Recognize external, LZ4, custom and unknown without opening .mcc. */
    const unsigned codecs[] = {129, 130, 131, 132, 4, 127, 0, 42};
    for (unsigned c = 0; c < sizeof(codecs) / sizeof(codecs[0]); c++) {
        reset(fd, 3);
        slot(fd, 0, 2, 1, 1, codecs[c]);
        v = load(fd);
        CHECK(mc_region_entry(v, 0)->compression == codecs[c]);
        CHECK(!mc_region_entry(v, 0)->decodable);
        CHECK(mc_region_plan_create(v, &p) == -1 && errno == EOPNOTSUPP && !p);
        mc_region_index_free(v);
        mc_region_file_t *f = NULL;
        CHECK(mc_region_open(path, &f) == 0);
        uint8_t *raw = NULL;
        size_t n = 0;
        CHECK(mc_region_read_record(f, 0, &raw, &n) == 0);
        CHECK(n == 5 && raw[4] == codecs[c]);
        free(raw);
        CHECK(mc_region_close(f) == 0);
    }
    /* Pinned readers retain old complete inode across atomic pathname replacement. */
    reset(fd, 3);
    slot(fd, 0, 2, 1, 1, 3);
    mc_region_file_t *f = NULL;
    CHECK(mc_region_open(path, &f) == 0);
    char replacement[] = "/tmp/goatcore-region-new.XXXXXX";
    int nf = mkstemp(replacement);
    CHECK(nf >= 0);
    reset(nf, 3);
    slot(nf, 0, 2, 1, 2, 3);
    CHECK(rename(replacement, path) == 0);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_region_read_record(f, 0, &raw, &n) == 0 && n == 5);
    free(raw);
    CHECK(mc_region_close(f) == 0);
    CHECK(mc_region_open(path, &f) == 0);
    CHECK(mc_region_read_record(f, 0, &raw, &n) == 0 && n == 6);
    free(raw);
    CHECK(mc_region_read_record(f, 1, &raw, &n) == 1);
    CHECK(mc_region_read_record(f, 1024, &raw, &n) == -1);
    CHECK(ftruncate(nf, 8192) == 0);
    CHECK(mc_region_read_record(f, 0, &raw, &n) == -1 && errno == ESTALE);
    CHECK(mc_region_close(f) == 0);
    CHECK(close(nf) == 0);
    int32_t rx, rz;
    unsigned si;
    mc_region_coordinates(-1, -33, &rx, &rz, &si);
    CHECK(rx == -1 && rz == -2 && si == 1023);
    mc_region_coordinates(32, 31, &rx, &rz, &si);
    CHECK(rx == 1 && rz == 0 && si == 992);
    mc_region_coordinates(INT32_MIN, INT32_MAX, &rx, &rz, &si);
    CHECK(rx == -67108864 && rz == 67108863 && si == 992);
    CHECK(mc_region_index_load(fd, NULL) == -1);
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    CHECK(mc_region_open(path, &f) == 1 && f == NULL);
    printf("region storage: %u checks passed\n", checks);
    return 0;
}
