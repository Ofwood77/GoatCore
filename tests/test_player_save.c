#include "mc_player_store.h"
#include "mc_player_save.h"
#include "mc_net.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

/* Link-time fault injection affects only this disposable C executable. */
static int stage, sync_count, fault, interrupted;
enum { SHORT = 20, IO, FULL, FILE_SYNC, DIR_SYNC, RENAME, EINTR_SYNC, EINTR_RENAME };
int __real_fsync(int fd);
int __real_rename(const char *a, const char *b);
int __real_gzwrite(gzFile file, voidpc data, unsigned length);
static void cut(int point) { if (stage == point) { raise(SIGKILL); _exit(100); } }
int __wrap_gzwrite(gzFile file, voidpc data, unsigned length) {
    cut(1);
    if (fault == IO || fault == FULL) { errno = fault == IO ? EIO : ENOSPC; return 0; }
    int result = __real_gzwrite(file, data, fault == SHORT ? length / 2 : length);
    if (stage == 2) assert(gzflush(file, Z_SYNC_FLUSH) == Z_OK); /* partial gzip on disk, not just a RAM buffer */
    cut(2);
    return result;
}
int __wrap_fsync(int fd) {
    int which = ++sync_count;
    cut(which == 1 ? 3 : 7);
    if (fault == EINTR_SYNC && !interrupted++) { errno = EINTR; sync_count--; return -1; }
    if ((fault == FILE_SYNC && which == 1) || (fault == DIR_SYNC && which == 2)) { errno = EIO; return -1; }
    int result = __real_fsync(fd);
    cut(which == 1 ? 4 : 8);
    return result;
}
int __wrap_rename(const char *a, const char *b) {
    cut(5);
    if (fault == EINTR_RENAME && !interrupted++) { errno = EINTR; return -1; }
    if (fault == RENAME) { errno = EIO; return -1; }
    int result = __real_rename(a, b);
    cut(6);
    return result;
}
static void verify(const char *path, int selected, int count) {
    mc_player_data_t loaded;
    mc_player_data_init(&loaded);
    assert(mc_player_store_load(path, NULL, false, "SaveFault", &loaded) == 0);
    assert(loaded.inventory.selected_hotbar_slot == selected);
    assert(loaded.inventory.slots[36].count == count);
    assert(loaded.inventory.slots[37].count == (count == 7 ? 11 : 13));
    assert(loaded.experience_level == (count == 7 ? 17 : 27));
    mc_player_data_clear(&loaded);
}
static void generations(void) {
    mc_player_save_state_t s = {0};
    for (int i = 0; i < 10; i++) assert(!mc_player_save_mark(&s, i));
    assert(s.due_ms == MC_PLAYER_HOTBAR_FLUSH_MS);
    assert(!mc_player_save_due(&s, s.due_ms - 1));
    assert(mc_player_save_begin(&s) == 10 && !mc_player_save_begin(&s));
    assert(!mc_player_save_mark(&s, 100));
    assert(!mc_player_save_complete(&s, 10, true));
    assert(s.current == 11 && s.durable == 10 && mc_player_save_due(&s, s.due_ms));
    assert(mc_player_save_begin(&s) == 11);
    assert(mc_player_save_complete(&s, 10, true) == -1 && s.in_flight == 11);
    assert(!mc_player_save_complete(&s, 11, false) && s.durable == 10);
    assert(mc_player_save_begin(&s) == 11 && !mc_player_save_complete(&s, 11, true));
    assert(s.current == s.durable && !s.due_ms && !mc_player_save_due(&s, INT64_MAX));
    s.current = UINT64_MAX;
    assert(mc_player_save_mark(&s, 1) == -1 && errno == EOVERFLOW);
}
int main(void) {
    generations();
    char path[] = "/tmp/goatcore-player-crash-XXXXXX";
    assert(mkdtemp(path));
    mc_player_data_t old, next;
    mc_player_data_init(&old); mc_player_data_init(&next);
    snprintf(old.username, sizeof(old.username), "SaveFault");
    snprintf(next.username, sizeof(next.username), "SaveFault");
    old.inventory.selected_hotbar_slot = 1; next.inventory.selected_hotbar_slot = 4;
    int id = mc_minecraft_item_id("minecraft:stone");
    assert(!mc_slot_set_simple(&old.inventory.slots[36], id, 7));
    assert(!mc_slot_set_simple(&next.inventory.slots[36], id, 9));
    assert(!mc_slot_set_simple(&old.inventory.slots[37], id, 11));
    assert(!mc_slot_set_simple(&next.inventory.slots[37], id, 13));
    old.experience_level = 17; next.experience_level = 27;
    /* Nine actual process interruptions, before snapshot through dir sync.
     * They do not emulate loss of the kernel page cache or a power outage. */
    for (int point = 0; point <= 8; point++) {
        stage = fault = sync_count = 0;
        assert(mc_player_store_save(path, &old) == 0);
        char file[1024]; snprintf(file,sizeof(file),"%s/players/data/SaveFault.dat",path);
        uint8_t *before, *after; size_t before_len, after_len;
        assert(!read_file(file,&before,&before_len));
        pid_t child = fork(); assert(child >= 0);
        if (!child) { if (!point) { raise(SIGKILL); _exit(100); } stage = point; sync_count = 0; mc_player_store_save(path, &next); _exit(101); }
        int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        verify(path, point < 6 ? 1 : 4, point < 6 ? 7 : 9);
        assert(!read_file(file,&after,&after_len));
        if (point < 6) assert(before_len == after_len && !memcmp(before,after,before_len));
        free(before); free(after);
        verify(path, point < 6 ? 1 : 4, point < 6 ? 7 : 9);
    }
    for (int f = SHORT; f <= EINTR_RENAME; f++) {
        stage = fault = sync_count = interrupted = 0;
        assert(mc_player_store_save(path, &old) == 0);
        fault = f; sync_count = 0;
        int result = mc_player_store_save(path, &next);
        assert(result == (f == DIR_SYNC ? MC_PLAYER_SAVE_UNCERTAIN : f >= EINTR_SYNC ? 0 : -1));
        bool published = f == DIR_SYNC || f >= EINTR_SYNC;
        verify(path, published ? 4 : 1, published ? 9 : 7);
        fault = 0; sync_count = 0;
        assert(mc_player_store_save(path, &next) == 0);
        verify(path, 4, 9);
    }
    mc_player_data_clear(&old); mc_player_data_clear(&next);
    printf("player RAM bases: player=%zu connection=%zu generation=%zu bytes; dynamic slots measured separately\n",sizeof(mc_player_data_t),sizeof(mc_conn_t),sizeof(mc_player_save_state_t));
    puts("player saves: generations, 9 SIGKILL stages, 8 short/error/EINTR cases passed");
    return 0;
}
