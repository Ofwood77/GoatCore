#ifdef NDEBUG
#error "Assertions are required by the transaction fault harness"
#endif
#include "mc_region_txn.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
static const char *kill_stage, *fault_stage, *phase = "";
static int fault_op, fault_errno, remaining, skip_matches;
static char base[4096];
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_pwrite(int, const void *, size_t, off_t);
int __real_fsync(int);
int __real_unlinkat(int, const char *, int);
int __real_ftruncate(int, off_t);
int __real_close(int);
int __real_renameat(int, const char *, int, const char *);
void *__real_realloc(void *, size_t);

void mc_region_txn_test_hook(const char *stage) {
    phase = stage;
    if (kill_stage && !strcmp(stage, kill_stage)) {
        kill(getpid(), SIGKILL);
        _exit(99);
    }
}
static bool inject(int op) {
    if (remaining && fault_stage && !strcmp(phase, fault_stage) && fault_op == op) {
        if (skip_matches) {
            skip_matches--;
            return false;
        }
        remaining--;
        errno = fault_errno;
        return true;
    }
    return false;
}
ssize_t __wrap_write(int fd, const void *p, size_t n) {
    if (inject(1)) {
        if (fault_errno == -1)
            return __real_write(fd, p, n > 7 ? 7 : n);
        if (!fault_errno)
            return 0;
        return -1;
    }
    return __real_write(fd, p, n);
}
ssize_t __wrap_pwrite(int fd, const void *p, size_t n, off_t pos) {
    if (kill_stage &&
        ((!strcmp(kill_stage, "data_partial") && !strcmp(phase, "data_begin")) ||
         (!strcmp(kill_stage, "header_partial") && !strcmp(phase, "header_begin")) ||
         (!strcmp(kill_stage, "recovery_partial") && !strcmp(phase, "recovery_header_begin")))) {
        ssize_t r = __real_pwrite(fd, p, n > 17 ? 17 : n, pos);
        assert(r > 0);
        kill(getpid(), SIGKILL);
        _exit(99);
    }
    if (inject(2)) {
        if (fault_errno == -1)
            return __real_pwrite(fd, p, n > 7 ? 7 : n, pos);
        if (!fault_errno)
            return 0;
        return -1;
    }
    return __real_pwrite(fd, p, n, pos);
}
int __wrap_fsync(int fd) {
    if (inject(3))
        return -1;
    return __real_fsync(fd);
}
int __wrap_unlinkat(int fd, const char *name, int flags) {
    if (inject(4))
        return -1;
    return __real_unlinkat(fd, name, flags);
}
int __wrap_ftruncate(int fd, off_t n) {
    if (inject(5))
        return -1;
    return __real_ftruncate(fd, n);
}
void *__wrap_realloc(void *p, size_t n) {
    if (inject(6))
        return NULL;
    return __real_realloc(p, n);
}
int __wrap_close(int fd) {
    if (inject(7)) {
        int e = errno;
        int r = __real_close(fd);
        assert(r == 0);
        errno = e;
        return -1;
    }
    return __real_close(fd);
}
int __wrap_renameat(int a, const char *b, int c, const char *d) {
    if (inject(8))
        return -1;
    return __real_renameat(a, b, c, d);
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static size_t payload(uint8_t out[64], unsigned value) {
    size_t n = 0;
    out[n++] = 10;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 3;
    out[n++] = 0;
    out[n++] = 11;
    memcpy(out + n, "DataVersion", 11);
    n += 11;
    put32(out + n, 4788);
    n += 4;
    out[n++] = 3;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = 'v';
    put32(out + n, value);
    n += 4;
    out[n++] = 8;
    out[n++] = 0;
    out[n++] = 6;
    memcpy(out + n, "opaque", 6);
    n += 6;
    out[n++] = 0;
    out[n++] = 4;
    memcpy(out + n, "S2!?", 4);
    n += 4;
    out[n++] = 0;
    return n;
}
static void path(char out[4096], const char *root, const char *name) {
    int n = snprintf(out, 4096, "%s/%s", root, name);
    CHECK(n > 0 && n < 4096);
}
static void copy(const char *src, const char *dst) {
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(in >= 0 && out >= 0);
    uint8_t buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        CHECK(mc_file_write_all(out, buf, (size_t)n) == 0);
    CHECK(n == 0);
    CHECK(close(in) == 0);
    CHECK(close(out) == 0);
}
static void make_root(char root[4096], unsigned chunks, bool holes) {
    int n = snprintf(root, 4096, "%s/case-XXXXXX", base);
    CHECK(n > 0 && n < 4096 && mkdtemp(root));
    char p[4096];
    path(p, root, MC_REGION_TXN_MARKER);
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(mc_file_write_all(fd, MC_REGION_TXN_MARKER_TEXT, strlen(MC_REGION_TXN_MARKER_TEXT)) == 0);
    CHECK(close(fd) == 0);
    path(p, root, "r.-1.-1.mca");
    fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    unsigned sectors = 2 + chunks * (holes ? 2 : 1);
    CHECK(ftruncate(fd, (off_t)sectors * 4096) == 0);
    uint8_t header[8192] = {0}, buf[69];
    for (unsigned i = 0; i < chunks; i++) {
        unsigned off = 2 + i * (holes ? 2 : 1);
        size_t len = payload(buf + 5, 1000 + i);
        put32(header + 4 * i, off << 8 | 1);
        put32(header + 4096 + 4 * i, 123456 + i);
        put32(buf, (uint32_t)len + 1);
        buf[4] = 3;
        CHECK(mc_file_pwrite_all(fd, buf, len + 5, (off_t)off * 4096) == 0);
    }
    CHECK(mc_file_pwrite_all(fd, header, 8192, 0) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
}
static mc_region_txn_store_t *opened(const char *root) {
    mc_region_txn_store_t *s = NULL;
    CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == 0 && s);
    return s;
}
static int change(mc_region_txn_store_t *s, bool full) {
    uint8_t a[64], b[64], c[64];
    unsigned last = full ? 1023 : 3, mid = full ? 511 : 1;
    mc_region_update_t updates[] = {
        {last, c, payload(c, 9000 + last)}, {0, a, payload(a, 9000)}, {mid, b, payload(b, 9000 + mid)}};
    return mc_region_txn_write(s, -1, -1, updates, 3);
}
static void expect(const char *root, unsigned chunks, bool newer, bool full) {
    char p[4096];
    path(p, root, "r.-1.-1.mca");
    mc_region_file_t *f = NULL;
    CHECK(mc_region_open(p, &f) == 0);
    CHECK(mc_region_stats(mc_region_file_index(f))->chunks == chunks);
    for (unsigned i = 0; i < chunks; i++) {
        bool modified = i == 0 || i == (full ? 511 : 1) || i == (full ? 1023 : 3);
        uint8_t expected[64], *raw = NULL;
        size_t len = payload(expected, (newer && modified ? 9000 : 1000) + i), n = 0;
        CHECK(mc_anvil_read_chunk_indexed(f, i, &raw, &n) == 0);
        CHECK(n == len && !memcmp(raw, expected, n));
        free(raw);
        if (!modified || !newer)
            CHECK(mc_region_entry(mc_region_file_index(f), i)->timestamp == 123456 + i);
    }
    CHECK(mc_region_close(f) == 0);
}
static void recover(const char *root) {
    mc_region_txn_store_t *s = opened(root);
    CHECK(mc_region_txn_close(s) == 0);
}
static void killed_write(const char *root, const char *stage) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        mc_region_txn_store_t *s = opened(root);
        kill_stage = stage;
        change(s, false);
        _exit(77);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
}
static void evidence(const char *root) {
    char src[4096], dst[4096];
    path(src, root, "r.-1.-1.mca");
    path(dst, root, "before-recovery.mca");
    copy(src, dst);
    path(src, root, "r.-1.-1.mca.s2-journal");
    if (access(src, F_OK) == 0) {
        path(dst, root, "journal.evidence");
        copy(src, dst);
    }
}
/* Test-only adapter used by the existing isolated Vanilla runner. The marker
 * and explicit opt-in are mandatory; recover BEFORE Vanilla is started. */
static int native_mode(int argc, char **argv) {
    if (argc < 6 || strcmp(argv[1], "--experimental-disposable") ||
        (strcmp(argv[2], "rewrite-region") && strcmp(argv[2], "crash-region")))
        return 2;
    const char *file = argv[3], *slash = strrchr(file, '/');
    if (!slash || slash == file)
        return 2;
    char *root = strndup(file, (size_t)(slash - file));
    CHECK(root);
    const char *name = slash + 1, *start = name + 2;
    char *end;
    if (strncmp(name, "r.", 2))
        return 2;
    errno = 0;
    long rx = strtol(start, &end, 10);
    if (errno || end == start || *end != '.' || rx < INT32_MIN || rx > INT32_MAX)
        return 2;
    start = end + 1;
    errno = 0;
    long rz = strtol(start, &end, 10);
    if (errno || end == start || strcmp(end, ".mca") || rz < INT32_MIN || rz > INT32_MAX)
        return 2;
    errno = 0;
    long lx = strtol(argv[4], &end, 10);
    if (errno || *end || end == argv[4] || lx < 0 || lx > 31)
        return 2;
    errno = 0;
    long lz = strtol(argv[5], &end, 10);
    if (errno || *end || end == argv[5] || lz < 0 || lz > 31)
        return 2;
    mc_region_txn_store_t *s = opened(root);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_anvil_read_chunk_nbt(file, (int)lx, (int)lz, &raw, &n) == 0 && mc_region_txn_close(s) == 0);
    mc_region_update_t update = {(unsigned)(lx + lz * 32), raw, n};
    if (!strcmp(argv[2], "crash-region")) {
        if (argc != 7)
            return 2;
        s = opened(root);
        kill_stage = argv[6];
        int rc = mc_region_txn_write(s, (int32_t)rx, (int32_t)rz, &update, 1);
        free(raw);
        mc_region_txn_close(s);
        free(root);
        return rc ? 1 : 3;
    }
    if (argc != 6)
        return 2;
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        s = opened(root);
        kill_stage = "header_partial";
        mc_region_txn_write(s, (int32_t)rx, (int32_t)rz, &update, 1);
        _exit(77);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    recover(root);
    recover(root);
    uint8_t *after = NULL;
    size_t after_n = 0;
    CHECK(mc_anvil_read_chunk_nbt(file, (int)lx, (int)lz, &after, &after_n) == 0);
    CHECK(after_n == n && !memcmp(after, raw, n));
    free(after);
    free(raw);
    free(root);
    printf("{\"result\":0,\"nbt_bytes\":%zu,\"recovered_crash\":\"header_partial\",\"idempotent\":true}\n",
           n);
    return 0;
}
static void *foreign_thread(void *arg) {
    mc_region_txn_store_t *s = arg;
    assert(change(s, false) == -1 && errno == EPERM);
    assert(mc_region_txn_close(s) == -1 && errno == EPERM);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc > 1)
        return native_mode(argc, argv);
    const char *parent = getenv("MC_REGION_TXN_TEST_ROOT");
    if (!parent)
        parent = "/tmp";
    int n = snprintf(base, sizeof(base), "%s/s2-XXXXXX", parent);
    CHECK(n > 0 && n < (int)sizeof(base) && mkdtemp(base));
    printf("S2 artifacts: %s\n", base);
    fflush(stdout);
    char root[4096], p[4096], q[4096];
    make_root(root, 4, true);
    mc_region_txn_store_t *s = opened(root), *other = NULL;
    CHECK(mc_region_txn_open(root, 0, &other) == -1 && !other);
    CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &other) == -1 && !other);
    pid_t owner = fork();
    CHECK(owner >= 0);
    if (!owner) {
        mc_region_txn_store_t *x = NULL;
        _exit(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &x) == -1 ? 0 : 1);
    }
    int status;
    CHECK(waitpid(owner, &status, 0) == owner && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, foreign_thread, s) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(change(s, false) == 0);
    CHECK(change(s, false) == 0);
    CHECK(mc_region_txn_close(s) == 0);
    expect(root, 4, true, false);
    recover(root);
    make_root(root, 1024, true);
    s = opened(root);
    CHECK(change(s, true) == 0);
    CHECK(mc_region_txn_close(s) == 0);
    expect(root, 1024, true, true);
    /* New region, negative and extreme region names, sequential regions. */
    make_root(root, 0, false);
    s = opened(root);
    uint8_t raw[64];
    mc_region_update_t one = {1023, raw, payload(raw, 42)};
    CHECK(mc_region_txn_write(s, INT32_MIN, INT32_MAX, &one, 1) == 0);
    CHECK(mc_region_txn_write(s, 0, -2, &one, 1) == 0);
    CHECK(mc_region_txn_close(s) == 0);
    recover(root);
    const char *stages[] = {"before_journal", "journal_partial",  "journal_durable", "data_partial",
                            "data_written",   "data_synced",      "header_begin",    "header_partial",
                            "header_written", "region_synced",    "before_commit",   "transaction_committed",
                            "before_cleanup", "journal_unlinked", "committed"};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        make_root(root, 4, i % 2);
        killed_write(root, stages[i]);
        evidence(root);
        bool newer = !strcmp(stages[i], "transaction_committed") || !strcmp(stages[i], "before_cleanup") ||
                     !strcmp(stages[i], "journal_unlinked") || !strcmp(stages[i], "committed");
        recover(root);
        expect(root, 4, newer, false);
        recover(root);
        expect(root, 4, newer, false);
        printf("crash stage=%s recovered=%s idempotent=1\n", stages[i], newer ? "new" : "old");
    }
    const char *recover_stages[] = {"recovery_begin",   "recovery_partial",       "recovery_header_written",
                                    "recovery_synced",  "recovery_state_durable", "recovery_unlinked",
                                    "recovery_complete"};
    for (int redo = 0; redo < 2; redo++) {
        for (unsigned i = 0; i < sizeof(recover_stages) / sizeof(recover_stages[0]); i++) {
            make_root(root, 4, true);
            killed_write(root, redo ? "transaction_committed" : "header_written");
            pid_t pid = fork();
            CHECK(pid >= 0);
            if (!pid) {
                kill_stage = recover_stages[i];
                recover(root);
                _exit(77);
            }
            int st;
            CHECK(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
            evidence(root);
            recover(root);
            expect(root, 4, redo, false);
            recover(root);
            expect(root, 4, redo, false);
            printf("recovery crash stage=%s outcome=%s idempotent=1\n", recover_stages[i],
                   redo ? "new" : "old");
        }
    }
    /* Arbitrarily torn header with valid undo must not prevent recovery. */
    make_root(root, 4, true);
    killed_write(root, "header_written");
    path(p, root, "r.-1.-1.mca");
    int fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    uint8_t garbage[8192];
    memset(garbage, 0xa5, sizeof(garbage));
    CHECK(mc_file_pwrite_all(fd, garbage, sizeof(garbage), 0) == 0);
    CHECK(close(fd) == 0);
    evidence(root);
    recover(root);
    expect(root, 4, false, false);
    recover(root);
    /* Old immutable journal must not overwrite a newer generation. */
    make_root(root, 4, true);
    killed_write(root, "header_written");
    path(p, root, "r.-1.-1.mca.s2-journal");
    path(q, root, "old-journal.saved");
    copy(p, q);
    recover(root);
    s = opened(root);
    CHECK(change(s, false) == 0);
    CHECK(mc_region_txn_close(s) == 0);
    copy(q, p);
    CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == -1 && !s);
    expect(root, 4, true, false);
    CHECK(access(p, F_OK) == 0);
    /* Truncated/corrupt journal and state: preserve all files and fail closed. */
    for (int bad = 0; bad < 3; bad++) {
        make_root(root, 4, true);
        killed_write(root, "header_written");
        path(p, root, bad == 2 ? "r.-1.-1.mca.s2-state" : "r.-1.-1.mca.s2-journal");
        fd = open(p, O_RDWR);
        CHECK(fd >= 0);
        if (!bad)
            CHECK(ftruncate(fd, 127) == 0);
        else {
            uint8_t byte = 255;
            CHECK(mc_file_pwrite_all(fd, &byte, 1, bad == 2 ? 96 : 128) == 0);
        }
        CHECK(close(fd) == 0);
        evidence(root);
        CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == -1 && !s);
        CHECK(access(p, F_OK) == 0);
    }
    /* Malformed canonical journal names, including overflowing coordinates. */
    make_root(root, 4, true);
    path(p, root, "r.99999999999999999999999999999.0.mca.s2-journal");
    fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == -1 && !s);
    const struct {
        const char *stage;
        int op;
    } faults[] = {{"state_begin", 1},      {"state_begin", 3},     {"journal_partial", 1},
                  {"journal_partial", 3},  {"journal_durable", 5}, {"data_begin", 2},
                  {"data_written", 3},     {"header_begin", 2},    {"header_written", 3},
                  {"before_commit", 1},    {"before_commit", 3},   {"before_commit", 8},
                  {"region_synced", 7},    {"committed", 7},       {"before_cleanup", 4},
                  {"journal_unlinked", 3}, {"prepare_begin", 6}};
    const int errors[] = {EINTR, EIO, ENOSPC, EDQUOT};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
        for (unsigned k = 0; k < 4; k++) {
            make_root(root, 4, false);
            s = opened(root);
            phase = "";
            fault_stage = faults[i].stage;
            fault_op = faults[i].op;
            fault_errno = errors[k];
            remaining = 1;
            if (fault_op == 6)
                fault_errno = ENOMEM;
            int rc = change(s, false);
            CHECK(!remaining);
            fault_stage = NULL;
            bool success = errors[k] == EINTR && fault_op != 6 && fault_op != 7;
            CHECK(success ? rc == 0 : rc < 0);
            CHECK(mc_region_txn_close(s) == 0);
            recover(root);
            bool newer = success || !strcmp(faults[i].stage, "journal_unlinked") ||
                         !strcmp(faults[i].stage, "before_cleanup") || !strcmp(faults[i].stage, "committed");
            expect(root, 4, newer, false);
            recover(root);
            printf("error stage=%s op=%d errno=%d result=%d recovered=%s\n", faults[i].stage, fault_op,
                   fault_errno, rc, newer ? "new" : "old");
        }
    for (int op = 1; op <= 2; op++)
        for (int kind = 0; kind < 2; kind++) {
            make_root(root, 4, false);
            s = opened(root);
            phase = "";
            fault_stage = op == 1 ? "journal_partial" : "header_begin";
            fault_op = op;
            fault_errno = kind ? -1 : 0;
            remaining = 1;
            int rc = change(s, false);
            CHECK(!remaining);
            fault_stage = NULL;
            CHECK(kind ? rc == 0 : rc < 0);
            CHECK(mc_region_txn_close(s) == 0);
            recover(root);
            expect(root, 4, kind, false);
        }
    /* A replay of the SAME committed generation must preserve the new image. */
    make_root(root, 4, true);
    killed_write(root, "transaction_committed");
    path(p, root, "r.-1.-1.mca.s2-journal");
    path(q, root, "committed-journal.saved");
    copy(p, q);
    recover(root);
    expect(root, 4, true, false);
    copy(q, p);
    recover(root);
    expect(root, 4, true, false);
    recover(root);
    /* Commit record directory-sync failure: visible outcome is new on SIGKILL,
     * whereas a power loss could retain old/new state before acknowledgement. */
    for (unsigned which = 0; which < 2; which++) {
        make_root(root, 4, false);
        s = opened(root);
        phase = "";
        fault_stage = which ? "before_commit" : "journal_partial";
        fault_op = 3;
        fault_errno = EIO;
        remaining = 1;
        skip_matches = 1;
        CHECK(change(s, false) == -2 && !remaining && !skip_matches);
        fault_stage = NULL;
        CHECK(mc_region_txn_close(s) == 0);
        recover(root);
        expect(root, 4, which != 0, false);
    }
    const struct {
        const char *stage;
        int op;
    } recovery_faults[] = {{"recovery_header_begin", 2},
                           {"recovery_header_written", 3},
                           {"recovery_synced", 1},
                           {"recovery_state_durable", 4},
                           {"recovery_unlinked", 3}};
    for (unsigned i = 0; i < sizeof(recovery_faults) / sizeof(recovery_faults[0]); i++)
        for (unsigned k = 0; k < 4; k++) {
            make_root(root, 4, false);
            killed_write(root, "header_written");
            phase = "";
            fault_stage = recovery_faults[i].stage;
            fault_op = recovery_faults[i].op;
            fault_errno = errors[k];
            remaining = 1;
            int rc = mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s);
            CHECK(!remaining);
            fault_stage = NULL;
            CHECK(errors[k] == EINTR ? rc == 0 : rc == -1);
            if (!rc)
                CHECK(mc_region_txn_close(s) == 0);
            recover(root);
            expect(root, 4, false, false);
            recover(root);
            printf("recovery error stage=%s errno=%d preserved=1\n", recovery_faults[i].stage, errors[k]);
        }
    /* Reject damaged live payloads, not just a damaged header/journal. */
    make_root(root, 4, true);
    killed_write(root, "header_written");
    path(p, root, "r.-1.-1.mca");
    fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    uint8_t bad_payload = 0x7f;
    CHECK(mc_file_pwrite_all(fd, &bad_payload, 1, 6 * 4096 + 9) == 0 && close(fd) == 0);
    CHECK(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == -1 && !s);
    path(p, root, "r.-1.-1.mca.s2-journal");
    CHECK(access(p, F_OK) == 0);
    /* Maximum batch size, all 1024 slots, deterministic ordering. */
    make_root(root, 1024, true);
    s = opened(root);
    mc_region_update_t *all = calloc(1024, sizeof(*all));
    uint8_t (*payloads)[64] = calloc(1024, 64);
    CHECK(all && payloads);
    for (unsigned i = 0; i < 1024; i++)
        all[1023 - i] = (mc_region_update_t){i, payloads[i], payload(payloads[i], 20000 + i)};
    CHECK(mc_region_txn_write(s, -1, -1, all, 1024) == 0 && mc_region_txn_close(s) == 0);
    path(p, root, "r.-1.-1.mca");
    mc_region_file_t *fullfile = NULL;
    CHECK(mc_region_open(p, &fullfile) == 0);
    for (unsigned i = 0; i < 1024; i++) {
        uint8_t *got = NULL;
        size_t size = 0;
        CHECK(mc_anvil_read_chunk_indexed(fullfile, i, &got, &size) == 0);
        CHECK(size == all[1023 - i].length && !memcmp(got, payloads[i], size));
        free(got);
    }
    CHECK(mc_region_close(fullfile) == 0);
    free(all);
    free(payloads);
    recover(root);
    /* Unsupported regions remain byte-for-byte unchanged, including other slots. */
    const unsigned codecs[] = {4, 127, 42, 130};
    for (unsigned i = 0; i < 4; i++) {
        make_root(root, 4, false);
        path(p, root, "r.-1.-1.mca");
        fd = open(p, O_RDWR);
        CHECK(fd >= 0);
        uint8_t codec = codecs[i];
        CHECK(mc_file_pwrite_all(fd, &codec, 1, 2 * 4096 + 4) == 0);
        CHECK(close(fd) == 0);
        path(q, root, "before.mca");
        copy(p, q);
        s = opened(root);
        CHECK(change(s, false) == -1 && errno == EOPNOTSUPP);
        CHECK(mc_region_txn_close(s) == 0);
        FILE *a = fopen(p, "rb"), *b = fopen(q, "rb");
        CHECK(a && b);
        int x, y;
        do {
            x = fgetc(a);
            y = fgetc(b);
            CHECK(x == y);
        } while (x != EOF);
        CHECK(fclose(a) == 0 && fclose(b) == 0);
    }
    /* Duplicate updates / incompressible oversized chunk fail without a journal. */
    make_root(root, 4, false);
    s = opened(root);
    mc_region_update_t dup[] = {one, one};
    CHECK(mc_region_txn_write(s, -1, -1, dup, 2) == -1);
    CHECK(mc_region_txn_close(s) == 0);
    expect(root, 4, false, false);
    size_t big = 1100000;
    uint8_t *noise = malloc(big);
    CHECK(noise);
    uint32_t rng = 1234567;
    for (size_t i = 0; i < big; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        noise[i] = (uint8_t)rng;
    }
    one = (mc_region_update_t){0, noise, big};
    s = opened(root);
    CHECK(mc_region_txn_write(s, -1, -1, &one, 1) == -1 && errno == EFBIG);
    CHECK(mc_region_txn_close(s) == 0);
    free(noise);
    expect(root, 4, false, false);
    printf("region transactions: %u checks passed\n", checks);
    return 0;
}
