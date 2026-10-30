#include "mc_atomic_file.h"
#include "mc_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_uint_fast64_t temporary_sequence;

static int sync_fd(int fd) {
    int rc;
    do {
        rc = fsync(fd);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

int mc_file_write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size > SSIZE_MAX ? SSIZE_MAX : size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

int mc_file_pwrite_all(int fd, const void *data, size_t size, off_t offset) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = pwrite(fd, p, size > SSIZE_MAX ? SSIZE_MAX : size, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        p += n;
        size -= (size_t)n;
        offset += n;
    }
    return 0;
}

int mc_file_pread_all(int fd, void *data, size_t size, off_t offset) {
    unsigned char *p = data;
    while (size) {
        ssize_t n = pread(fd, p, size > SSIZE_MAX ? SSIZE_MAX : size, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        p += n;
        size -= (size_t)n;
        offset += n;
    }
    return 0;
}

void mc_atomic_file_abort(mc_atomic_file_t *file) {
    int saved = errno;
    /* Linux close releases the descriptor even on EINTR: retrying could close
     * a descriptor reused by another thread. Cleanup never touches the target. */
    if (file->temp_fd >= 0 && close(file->temp_fd) != 0)
        log_error("atomic cleanup close failed path=%s errno=%d", file->path, errno);
    file->temp_fd = -1;
    if (file->temporary[0] && file->dir_fd >= 0) {
        int rc;
        do {
            rc = unlinkat(file->dir_fd, file->temporary, 0);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            log_error("atomic cleanup unlink failed path=%s temporary=%s errno=%d", file->path,
                      file->temporary, errno);
    }
    file->temporary[0] = 0;
    if (file->dir_fd >= 0 && close(file->dir_fd) != 0)
        log_error("atomic cleanup directory close failed path=%s errno=%d", file->path, errno);
    file->dir_fd = -1;
    errno = saved;
}

int mc_atomic_file_begin(mc_atomic_file_t *file, const char *path) {
    *file = (mc_atomic_file_t){.dir_fd = -1, .temp_fd = -1, .path = path};
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (!*base || strlen(base) >= sizeof(file->target)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!strcmp(base, ".") || !strcmp(base, "..")) {
        errno = EINVAL;
        return -1;
    }
    strcpy(file->target, base);
    char *parent = slash ? strndup(path, slash == path ? 1 : (size_t)(slash - path)) : strdup(".");
    if (!parent)
        return -1;
    do {
        file->dir_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (file->dir_fd < 0 && errno == EINTR);
    int saved = errno;
    free(parent);
    errno = saved;
    if (file->dir_fd < 0)
        return -1;

    struct stat old;
    int rc;
    do {
        rc = fstatat(file->dir_fd, base, &old, AT_SYMLINK_NOFOLLOW);
    } while (rc < 0 && errno == EINTR);
    bool exists = rc == 0;
    if ((rc < 0 && errno != ENOENT) || (exists && !S_ISREG(old.st_mode))) {
        if (exists)
            errno = EINVAL;
        goto fail;
    }
    for (int attempt = 0; attempt < 1024; attempt++) {
        uint64_t seq = atomic_fetch_add_explicit(&temporary_sequence, 1, memory_order_relaxed);
        int n = snprintf(file->temporary, sizeof(file->temporary), "%s.tmp.%ld.%llu", base, (long)getpid(),
                         (unsigned long long)seq);
        if (n < 0 || (size_t)n >= sizeof(file->temporary)) {
            file->temporary[0] = 0;
            errno = ENAMETOOLONG;
            goto fail;
        }
        do {
            file->temp_fd =
                openat(file->dir_fd, file->temporary, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        } while (file->temp_fd < 0 && errno == EINTR);
        if (file->temp_fd >= 0)
            break;
        file->temporary[0] = 0; /* This name belongs to someone else on EEXIST. */
        if (errno != EEXIST)
            goto fail;
    }
    if (file->temp_fd < 0) {
        errno = EEXIST;
        goto fail;
    }
    if (exists) {
        struct stat current;
        do {
            rc = fstat(file->temp_fd, &current);
        } while (rc < 0 && errno == EINTR);
        if (rc != 0)
            goto fail;
        if (current.st_uid != old.st_uid || current.st_gid != old.st_gid) {
            do {
                rc = fchown(file->temp_fd, old.st_uid, old.st_gid);
            } while (rc < 0 && errno == EINTR);
            if (rc != 0)
                goto fail;
        }
        do {
            rc = fchmod(file->temp_fd, old.st_mode & 0777);
        } while (rc < 0 && errno == EINTR);
        if (rc != 0)
            goto fail;
    }
    return 0;
fail:
    mc_atomic_file_abort(file);
    return -1;
}

int mc_atomic_file_commit(mc_atomic_file_t *file) {
    int result = -1;
    mc_save_clock_t span = mc_save_profile_begin();
    if (sync_fd(file->temp_fd) != 0)
        goto fail;
    mc_save_profile_end("atomic_file_sync", 0, 0, span, 0, 0);
    int fd = file->temp_fd;
    file->temp_fd = -1;
    if (close(fd) != 0)
        goto fail;
    span = mc_save_profile_begin();
    int rc;
    do {
        rc = renameat(file->dir_fd, file->temporary, file->dir_fd, file->target);
    } while (rc < 0 && errno == EINTR);
    if (rc != 0)
        goto fail;
    file->temporary[0] = 0; /* It is now the destination. Never unlink/restore it. */
    result = MC_SAVE_DURABILITY_UNCERTAIN;
    mc_save_profile_end("atomic_rename", 0, 0, span, 0, 0);
    span = mc_save_profile_begin();
    if (sync_fd(file->dir_fd) != 0)
        goto fail;
    mc_save_profile_end("atomic_directory_sync", 0, 0, span, 0, 0);
    fd = file->dir_fd;
    file->dir_fd = -1;
    if (close(fd) != 0)
        goto fail;
    return 0;
fail:
    if (result == MC_SAVE_DURABILITY_UNCERTAIN) {
        int saved = errno;
        log_error("atomic replacement visible, durability uncertain path=%s errno=%d (%s)", file->path, saved,
                  strerror(saved));
        errno = saved;
    }
    mc_atomic_file_abort(file);
    return result;
}

int mc_atomic_file_lock(const char *path) {
    size_t size = strlen(path) + sizeof(".lock");
    char *name = malloc(size);
    if (!name)
        return -1;
    snprintf(name, size, "%s.lock", path);
    int fd;
    do {
        fd = open(name, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (fd < 0 && errno == EINTR);
    int saved = errno;
    free(name);
    errno = saved;
    if (fd < 0)
        return -1;
    struct stat st;
    int stat_rc;
    do {
        stat_rc = fstat(fd, &st);
    } while (stat_rc < 0 && errno == EINTR);
    if (stat_rc != 0 || !S_ISREG(st.st_mode)) {
        saved = stat_rc != 0 ? errno : EINVAL;
        if (close(fd) != 0)
            log_error("region lock close failed errno=%d", errno);
        errno = saved;
        return -1;
    }
    int rc;
    do {
        rc = flock(fd, LOCK_EX);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0)
        return fd;
    saved = errno;
    if (close(fd) != 0)
        log_error("region lock close failed errno=%d", errno);
    errno = saved;
    return -1;
}

int mc_persistence_mkdirs(const char *path, mode_t mode) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char *copy = strdup(path), *next = NULL;
    if (!copy)
        return -1;
    int fd;
    do {
        fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        free(copy);
        return -1;
    }
    int result = -1;
    for (char *part = strtok_r(copy, "/", &next); part; part = strtok_r(NULL, "/", &next)) {
        int rc;
        do {
            rc = mkdirat(fd, part, mode);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != EEXIST)
            goto done;
        int child;
        do {
            child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        } while (child < 0 && errno == EINTR);
        if (child < 0)
            goto done;
        if (sync_fd(child) != 0 || sync_fd(fd) != 0) {
            int saved = errno;
            if (close(child) != 0)
                log_error("new directory cleanup close failed errno=%d", errno);
            errno = saved;
            goto done;
        }
        int old = fd;
        fd = child;
        if (close(old) != 0)
            goto done;
    }
    result = 0;
done:;
    int saved = errno;
    if (close(fd) != 0 && result == 0) {
        result = -1;
        saved = errno;
    }
    free(copy);
    errno = saved;
    return result;
}
