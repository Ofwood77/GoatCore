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
static int output = -1;
static int (*real_fsync)(int);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static long long stamp(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void init(void) {
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    const char *path = getenv("MC_FSYNC_TRACE_PATH");
    if (path && *path) output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
}
int fsync(int fd) {
    pthread_once(&once, init);
    if (!real_fsync) { errno = ENOSYS; return -1; }
    if (output < 0) return real_fsync(fd);
    long long start = stamp(CLOCK_MONOTONIC), cpu = stamp(CLOCK_THREAD_CPUTIME_ID);
    int rc = real_fsync(fd), saved_errno = errno;
    long long end = stamp(CLOCK_MONOTONIC), used = stamp(CLOCK_THREAD_CPUTIME_ID) - cpu;
    char link[64], path[256], row[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path)-1);
    if (n < 0) n = 0;
    path[n] = 0;
    int len = snprintf(row, sizeof(row), "%lld,%lld,%lld,%ld,%d,%s\n", start, end, used, syscall(SYS_gettid), rc, path);
    if (len > 0 && (size_t)len < sizeof(row)) (void)write(output, row, (size_t)len);
    errno = saved_errno;
    return rc;
}
