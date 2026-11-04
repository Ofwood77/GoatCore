/* Linked only into mc_migrate_fault, never normal tool or mc_server. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static const char *failure;
static bool active, used, in_region, in_source_read, in_verify;
void mc_migration_test_hook(const char *phase) {
    if (!strcmp(phase, "region-write"))
        in_region = true;
    if (!strcmp(phase, "read-before"))
        in_source_read = true;
    if (!strcmp(phase, "verify"))
        in_verify = true;
    const char *wanted = getenv("MC_MIGRATION_TEST_PHASE");
    if (!wanted || strcmp(phase, wanted) || active)
        return;
    active = true;
    const char *requested_failure = getenv("MC_MIGRATION_TEST_ERROR");
    const char *ready = getenv("MC_MIGRATION_TEST_READY"), *release = getenv("MC_MIGRATION_TEST_RELEASE");
    if (ready && release) {
        int a = atoi(ready), b = atoi(release);
        char byte = 'R';
        ssize_t r;
        if (a < 3 || b < 3 || a == b)
            _exit(120);
        do {
            r = write(a, &byte, 1);
        } while (r < 0 && errno == EINTR);
        if (r != 1)
            _exit(121);
        do {
            r = read(b, &byte, 1);
        } while (r < 0 && errno == EINTR);
        if (r != 1)
            _exit(122);
    }
    failure = requested_failure;
}
static bool wants(const char *phase) {
    const char *p = getenv("MC_MIGRATION_TEST_PHASE");
    return !active && p && !strcmp(p, phase);
}
static bool inject(const char *operation) {
    if (!active || used || !failure || !strstr(failure, operation))
        return false;
    used = true;
    errno = strstr(failure, "ENOSPC") ? ENOSPC : strstr(failure, "EINTR") ? EINTR : EIO;
    return true;
}
ssize_t __real_pread(int, void *, size_t, off_t);
ssize_t __wrap_pread(int fd, void *data, size_t n, off_t offset) {
    const char *pause = in_verify ? "verify-read" : in_source_read ? "source-read-partial" : "";
    if (*pause && wants(pause)) {
        ssize_t read = __real_pread(fd, data, n > 32 ? 32 : n, offset);
        if (read > 0)
            mc_migration_test_hook(pause);
        return read;
    }
    if (inject("pread"))
        return -1;
    return __real_pread(fd, data, n, offset);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t n) {
    if (fd > 2 && in_region && wants("region-write-partial")) {
        ssize_t written = __real_write(fd, data, n > 32 ? 32 : n);
        if (written > 0)
            mc_migration_test_hook("region-write-partial");
        return written;
    }
    if (fd > 2 && inject("write")) {
        if (strstr(failure, "short"))
            return __real_write(fd, data, n > 1 ? n / 2 : n);
        return -1;
    }
    return __real_write(fd, data, n);
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
    if (in_region && wants("region-file-sync-before"))
        mc_migration_test_hook("region-file-sync-before");
    if (inject("fsync"))
        return -1;
    int rc = __real_fsync(fd);
    if (!rc && in_region && wants("region-file-sync-after"))
        mc_migration_test_hook("region-file-sync-after");
    return rc;
}
int __real_mkdirat(int, const char *, mode_t);
int __wrap_mkdirat(int fd, const char *path, mode_t mode) {
    if (inject("mkdir"))
        return -1;
    return __real_mkdirat(fd, path, mode);
}
int __real_renameat2(int, const char *, int, const char *, unsigned);
int __wrap_renameat2(int a, const char *b, int c, const char *d, unsigned flags) {
    if (inject("rename")) {
        if (strstr(failure, "after")) {
            int rc = __real_renameat2(a, b, c, d, flags);
            if (rc)
                return rc;
            errno = EIO;
        }
        return -1;
    }
    return __real_renameat2(a, b, c, d, flags);
}
