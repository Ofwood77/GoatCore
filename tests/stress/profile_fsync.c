/* Test-only LD_PRELOAD probe: preserve fsync's return value and errno.
 * Build on the remote host: cc -shared -fPIC -O2 -o profile_fsync.so
 * tests/stress/profile_fsync.c -ldl -pthread
 * Never preload this library into a main service. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/eventfd.h>
#include <stdbool.h>
#include <stdarg.h>
/* Network wakeups vastly outnumber world writes. Remember only descriptors
 * created as eventfd, clearing BEFORE close/replacement. Never cache file
 * classifications: a reused descriptor must be inspected again. */
#define WAKE_FDS 4096
static _Atomic bool wake_fd[WAKE_FDS];
static int file_fd(int fd) {
    if (fd >= 0 && fd < WAKE_FDS) atomic_store(&wake_fd[fd], false);
    return fd;
}
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    return file_fd((int)syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
int openat(int directory, const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    return file_fd((int)syscall(SYS_openat, directory, path, flags, mode));
}
int eventfd(unsigned int initial, int flags) {
    int fd = (int)syscall(SYS_eventfd2, initial, flags);
    if (fd >= 0 && fd < WAKE_FDS) atomic_store(&wake_fd[fd], true);
    return fd;
}
int close(int fd) {
    if (fd >= 0 && fd < WAKE_FDS) atomic_store(&wake_fd[fd], false);
    return (int)syscall(SYS_close, fd);
}
int dup2(int oldfd, int newfd) {
    if (newfd >= 0 && newfd < WAKE_FDS) atomic_store(&wake_fd[newfd], false);
    return (int)syscall(SYS_dup2, oldfd, newfd);
}
int dup3(int oldfd, int newfd, int flags) {
    if (newfd >= 0 && newfd < WAKE_FDS) atomic_store(&wake_fd[newfd], false);
    return (int)syscall(SYS_dup3, oldfd, newfd, flags);
}
static int output = -1;
static int (*real_fsync)(int);
static int (*real_fdatasync)(int);
static _Atomic unsigned long long sync_fsync_calls, sync_fdatasync_calls;
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off64_t);
static int io_output = -1;
static const char *io_root;
static size_t io_root_length;
static _Atomic unsigned long long io_calls, io_bytes, io_errors, io_dropped;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static const char *final_path;
static void final_usage(void) {
    if (!final_path || !*final_path) return;
    /* VmHWM belongs to the current executable. ru_maxrss can retain a
     * pre-exec parent's peak and must not be substituted for this value. */
    FILE *status = fopen("/proc/self/status", "r");
    unsigned long peak = 0;
    char line[256];
    if (status) {
        while (fgets(line, sizeof(line), status)) {
            unsigned long n;
            if (sscanf(line, "VmHWM: %lu kB", &n) == 1) peak = n;
        }
        fclose(status);
    }
    struct rusage usage;
    if (!peak || getrusage(RUSAGE_SELF, &usage) != 0) return;
    int fd = open(final_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return;
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); return; }
    fprintf(out, "{\"pid\":%ld,\"vmhwm_kb\":%lu,\"user_s\":%ld.%06ld,\"system_s\":%ld.%06ld",
            (long)getpid(), peak, (long)usage.ru_utime.tv_sec, (long)usage.ru_utime.tv_usec,
            (long)usage.ru_stime.tv_sec, (long)usage.ru_stime.tv_usec);
    if (io_output >= 0)
        fprintf(out, ",\"world_write_calls\":%llu,\"world_write_bytes\":%llu,\"world_write_errors\":%llu,\"write_trace_dropped\":%llu",
                atomic_load(&io_calls), atomic_load(&io_bytes), atomic_load(&io_errors), atomic_load(&io_dropped));
    fprintf(out, ",\"fsync_calls\":%llu,\"fdatasync_calls\":%llu", atomic_load(&sync_fsync_calls), atomic_load(&sync_fdatasync_calls));
    FILE *io = fopen("/proc/self/io", "r");
    if (io) {
        char key[64]; unsigned long long n;
        while (fgets(line, sizeof(line), io))
            if (sscanf(line, "%63[^:]: %llu", key, &n) == 2)
                fprintf(out, ",\"%s\":%llu", key, n);
        fclose(io);
    }
    fprintf(out, "}\n");
    fclose(out);
}
static long long stamp(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void init(void) {
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
    real_write = dlsym(RTLD_NEXT, "write");
    real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    const char *path = getenv("MC_FSYNC_TRACE_PATH");
    if (path && *path) output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    path = getenv("MC_IO_TRACE_PATH");
    io_root = getenv("MC_IO_TRACE_ROOT");
    if (path && *path && io_root && *io_root) {
        io_root_length = strlen(io_root);
        io_output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    }
    final_path = getenv("MC_PROC_FINAL_PATH");
    if (final_path && *final_path) (void)atexit(final_usage);
}
static void record_write(const char *call, int fd, size_t requested, ssize_t result, long long begin) {
    if (io_output < 0 || fd == io_output || fd == output)
        return;
    int saved = errno;
    char link[64], path[4096], row[4352];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = 0;
        if (!strncmp(path, io_root, io_root_length) && path[io_root_length] == '/') {
            atomic_fetch_add(&io_calls, 1);
            if (result > 0) atomic_fetch_add(&io_bytes, (unsigned long long)result);
            if (result < 0) atomic_fetch_add(&io_errors, 1);
            int len = snprintf(row, sizeof(row), "%lld,%lld,%ld,%s,%zu,%zd,%s\n", begin,
                    stamp(CLOCK_MONOTONIC), syscall(SYS_gettid), call, requested, result, path);
            if (len <= 0 || (size_t)len >= sizeof(row) || real_write(io_output, row, (size_t)len) != len)
                atomic_fetch_add(&io_dropped, 1);
        }
    }
    errno = saved;
}
ssize_t write(int fd, const void *data, size_t size) {
    int saved = errno;
    pthread_once(&once, init);
    if (!real_write) { errno = ENOSYS; return -1; }
    bool inspect = io_output >= 0 && !(fd >= 0 && fd < WAKE_FDS && atomic_load(&wake_fd[fd]));
    long long begin = inspect ? stamp(CLOCK_MONOTONIC) : 0;
    errno = saved;
    ssize_t rc = real_write(fd, data, size);
    if (inspect) record_write("write", fd, size, rc, begin);
    return rc;
}
ssize_t pwrite(int fd, const void *data, size_t size, off_t offset) {
    int saved = errno;
    pthread_once(&once, init);
    if (!real_pwrite) { errno = ENOSYS; return -1; }
    long long begin = io_output >= 0 ? stamp(CLOCK_MONOTONIC) : 0;
    errno = saved;
    ssize_t rc = real_pwrite(fd, data, size, offset);
    record_write("pwrite", fd, size, rc, begin);
    return rc;
}
ssize_t pwrite64(int fd, const void *data, size_t size, off64_t offset) {
    int saved = errno;
    pthread_once(&once, init);
    if (!real_pwrite64) { errno = ENOSYS; return -1; }
    long long begin = io_output >= 0 ? stamp(CLOCK_MONOTONIC) : 0;
    errno = saved;
    ssize_t rc = real_pwrite64(fd, data, size, offset);
    record_write("pwrite64", fd, size, rc, begin);
    return rc;
}
static int trace_sync(int fd, bool data_only) {
    pthread_once(&once, init);
    if (!(data_only ? real_fdatasync : real_fsync)) { errno = ENOSYS; return -1; }
    if (data_only) atomic_fetch_add(&sync_fdatasync_calls, 1); else atomic_fetch_add(&sync_fsync_calls, 1);
    if (output < 0) return data_only ? real_fdatasync(fd) : real_fsync(fd);
    long long start = stamp(CLOCK_MONOTONIC), cpu = stamp(CLOCK_THREAD_CPUTIME_ID);
    int rc = data_only ? real_fdatasync(fd) : real_fsync(fd), saved_errno = errno;
    long long end = stamp(CLOCK_MONOTONIC), used = stamp(CLOCK_THREAD_CPUTIME_ID) - cpu;
    char link[64], path[256], row[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path)-1);
    if (n < 0) n = 0;
    path[n] = 0;
    int len = snprintf(row, sizeof(row), "%lld,%lld,%lld,%ld,%d,%s\n", start, end, used, syscall(SYS_gettid), rc, path);
    if (len > 0 && (size_t)len < sizeof(row)) {
        ssize_t recorded = write(output, row, (size_t)len);
        (void)recorded; /* Caller verifies trace completeness against save spans. */
    }
    errno = saved_errno;
    return rc;
}

int fsync(int fd) { return trace_sync(fd, false); }
int fdatasync(int fd) { return trace_sync(fd, true); }
