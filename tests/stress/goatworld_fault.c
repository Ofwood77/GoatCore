/* G1 deterministic test-only LD_PRELOAD, including SQLite's shared-library I/O. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
static int spent, gated;
static int inside(int fd) {
    const char *root = getenv("G1_TEST_ROOT");
    if (!root || fd < 3)
        return 0;
    char link[64], path[4096];
    /* Avoid stdio and its writes while intercepting write. */
    size_t at = 0;
    const char *prefix = "/proc/self/fd/";
    while (prefix[at]) {
        link[at] = prefix[at];
        at++;
    }
    char digits[24];
    unsigned n = 0;
    int v = fd;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        link[at++] = digits[--n];
    link[at] = 0;
    ssize_t count = readlink(link, path, sizeof(path) - 1);
    if (count < 0)
        return 0;
    path[count] = 0;
    size_t len = strlen(root);
    return !strncmp(path, root, len) && (path[len] == '/' || path[len] == 0);
}
static int name_has(int fd, const char *part) {
    char link[64], path[4096];
    size_t at = 0;
    const char *p = "/proc/self/fd/";
    while (p[at]) {
        link[at] = p[at];
        at++;
    }
    char digits[24];
    unsigned n = 0;
    int v = fd;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        link[at++] = digits[--n];
    link[at] = 0;
    ssize_t len = readlink(link, path, sizeof(path) - 1);
    if (len < 0)
        return 0;
    path[len] = 0;
    return strstr(path, part) != NULL;
}
static int wants(const char *phase) {
    const char *wanted = getenv("G1_TEST_PHASE");
    return !gated && wanted && !strcmp(wanted, phase);
}
static void gate(const char *phase) {
    if (!wants(phase))
        return;
    gated = 1;
    const char *a = getenv("G1_TEST_READY"), *b = getenv("G1_TEST_RELEASE");
    if (a && b) {
        char c = 'R';
        ssize_t n;
        do {
            n = syscall(SYS_write, atoi(a), &c, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 1)
            _exit(124);
        do {
            n = syscall(SYS_read, atoi(b), &c, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 1)
            _exit(125);
    }
    if (setenv("G1_FAULT_ACTIVE", "1", 1))
        _exit(126);
}
static const char *inject(const char *operation, int fd) {
    const char *error = getenv("G1_TEST_ERROR");
    if (spent || !getenv("G1_FAULT_ACTIVE") || !error || !strstr(error, operation) || !inside(fd))
        return NULL;
    spent = 1;
    errno = strstr(error, "ENOSPC") ? ENOSPC : strstr(error, "EINTR") ? EINTR : EIO;
    return error;
}
ssize_t write(int fd, const void *data, size_t n) {
    if (inside(fd) && name_has(fd, ".idx.tmp.") && wants("index-partial")) {
        ssize_t written = syscall(SYS_write, fd, data, n > 1 ? n / 2 : n);
        if (written > 0)
            gate("index-partial");
        return written;
    }
    int saved = errno;
    const char *error = inject("write", fd);
    if (error) {
        if (strstr(error, "short"))
            return syscall(SYS_write, fd, data, n > 1 ? n / 2 : n);
        return -1;
    }
    errno = saved;
    return syscall(SYS_write, fd, data, n);
}
ssize_t pwrite(int fd, const void *data, size_t n, off_t offset) {
    if (inside(fd) && name_has(fd, ".data.") && offset >= 4096 &&
        (wants("data-partial") || wants("compact-data-partial"))) {
        ssize_t written = syscall(SYS_pwrite64, fd, data, n > 1 ? n / 2 : n, offset);
        if (written > 0)
            gate(wants("data-partial") ? "data-partial" : "compact-data-partial");
        return written;
    }
    if (inside(fd) && name_has(fd, ".idx.tmp.") && wants("index-partial")) {
        ssize_t written = syscall(SYS_pwrite64, fd, data, n > 1 ? n / 2 : n, offset);
        if (written > 0)
            gate("index-partial");
        return written;
    }
    int saved = errno;
    const char *error = inject("pwrite", fd);
    if (error) {
        if (strstr(error, "short"))
            return syscall(SYS_pwrite64, fd, data, n > 1 ? n / 2 : n, offset);
        return -1;
    }
    errno = saved;
    return syscall(SYS_pwrite64, fd, data, n, offset);
}
ssize_t pwrite64(int fd, const void *data, size_t n, off64_t offset) {
    return pwrite(fd, data, n, (off_t)offset);
}
int fsync(int fd) {
    int saved = errno;
    if (inject("fsync", fd))
        return -1;
    errno = saved;
    int rc = (int)syscall(SYS_fsync, fd);
    if (!rc && inside(fd) && name_has(fd, ".idx.tmp."))
        gate("index-file-synced");
    return rc;
}
int fdatasync(int fd) {
    int saved = errno;
    if (inject("fsync", fd))
        return -1;
    errno = saved;
    return (int)syscall(SYS_fdatasync, fd);
}
int renameat(int a, const char *b, int c, const char *d) {
    int known = inside(a) || inside(c);
    if (known && strstr(d, ".idx"))
        gate("index-rename-before");
    int rc = (int)syscall(SYS_renameat, a, b, c, d);
    if (!rc && known && strstr(d, ".idx"))
        gate("index-rename-after");
    return rc;
}
int unlinkat(int fd, const char *name, int flags) {
    int rc = (int)syscall(SYS_unlinkat, fd, name, flags);
    if (!rc && strstr(name, ".data."))
        gate("compact-unlinked");
    return rc;
}

ssize_t pread(int fd, void *data, size_t n, off_t offset) {
    int saved = errno;
    if (inject("pread", fd))
        return -1;
    errno = saved;
    return syscall(SYS_pread64, fd, data, n, offset);
}
ssize_t pread64(int fd, void *data, size_t n, off64_t offset) { return pread(fd, data, n, (off_t)offset); }
