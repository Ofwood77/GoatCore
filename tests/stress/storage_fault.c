/* Test-only Linux fault gate. Never preload into a main service. The Python
 * runner passes two pipes and marks its fresh disposable world explicitly. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static const char *root, *stage, *failure;
static int ready = -1, release_fd = -1;
static atomic_bool entered;
static bool enabled;
__attribute__((constructor)) static void setup(void) {
    root = getenv("MC_S5_FAULT_ROOT");
    stage = getenv("MC_S5_FAULT_STAGE");
    failure = getenv("MC_S5_FAULT_ERROR");
    const char *a = getenv("MC_S5_FAULT_READY"), *b = getenv("MC_S5_FAULT_RELEASE");
    if (!root || root[0] != '/' || !stage || !a || !b)
        return;
    char path[4096];
    struct stat st;
    if (snprintf(path, sizeof(path), "%s/.goatcore-s5-disposable", root) >= (int)sizeof(path) ||
        lstat(path, &st) || !S_ISREG(st.st_mode))
        _exit(120);
    ready = atoi(a);
    release_fd = atoi(b);
    if (ready < 3 || release_fd < 3 || ready == release_fd ||
        fcntl(ready, F_GETFD) < 0 || fcntl(release_fd, F_GETFD) < 0)
        _exit(121);
    enabled = true;
}
static bool region_fd(int fd, bool directory) {
    if (!enabled)
        return false;
    int saved = errno;
    char name[64], path[4096];
    snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(name, path, sizeof(path) - 1);
    bool match = false;
    if (n > 0) {
        path[n] = 0;
        size_t r = strlen(root);
        match = !strncmp(path, root, r) && path[r] == '/' &&
                (directory ? !strcmp(path + r, "/dimensions/minecraft/overworld/region")
                           : strstr(path + r, "/region/r.") && strstr(path + r, ".mca.tmp."));
    }
    errno = saved;
    return match;
}
static bool armed(void) {
    char name[4096];
    return snprintf(name, sizeof(name), "%s/.s5-fault-armed", root) < (int)sizeof(name) &&
           syscall(SYS_faccessat, AT_FDCWD, name, F_OK) == 0;
}
static bool gate(const char *point) {
    int saved = errno;
    if (!enabled || strcmp(stage, point) || !armed() || atomic_exchange(&entered, true)) {
        errno = saved;
        return false;
    }
    char byte = 'R';
    ssize_t rc;
    do { rc = syscall(SYS_write, ready, &byte, 1); } while (rc < 0 && errno == EINTR);
    if (rc != 1)
        _exit(122);
    do { rc = syscall(SYS_read, release_fd, &byte, 1); } while (rc < 0 && errno == EINTR);
    /* EOF also releases cleanup after a failed test; the controller owns kill. */
    errno = saved;
    return true;
}
static int error(void) {
    if (!failure || !*failure)
        return 0;
    errno = !strcmp(failure, "ENOSPC") ? ENOSPC : EIO;
    return -1;
}
ssize_t write(int fd, const void *data, size_t n) {
    if (region_fd(fd, false)) {
        if (!strcmp(stage, "partial-write") && !atomic_load(&entered) && armed()) {
            ssize_t used = syscall(SYS_write, fd, data, n > 32 ? 32 : n);
            if (used > 0 && gate("partial-write"))
                return error() ? -1 : used;
            return used;
        }
        if (gate("write-before") && error())
            return -1;
    }
    return syscall(SYS_write, fd, data, n);
}
int fsync(int fd) {
    bool directory = region_fd(fd, true), file = region_fd(fd, false);
    if ((directory && gate("dir-sync-before")) || (file && gate("file-sync-before")))
        if (error())
            return -1;
    if (enabled && failure && !strcmp(failure, "EIO-permanent") && atomic_load(&entered) &&
        ((directory && !strcmp(stage, "dir-sync-before")) ||
         (file && !strcmp(stage, "file-sync-before"))))
        return error();
    int rc = (int)syscall(SYS_fsync, fd);
    if (!rc) {
        if (directory)
            (void)gate("dir-sync-after");
        if (file)
            (void)gate("file-sync-after");
    }
    return rc;
}
int renameat(int olddir, const char *old, int newdir, const char *next) {
    bool match = region_fd(olddir, true) && strstr(old, ".mca.tmp.");
    if (match && gate("rename-before") && error())
        return -1;
    int rc = (int)syscall(SYS_renameat, olddir, old, newdir, next);
    if (!rc && match)
        (void)gate("rename-after");
    return rc;
}
