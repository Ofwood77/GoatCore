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
static int output = -1;
static int (*real_fsync)(int);
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
    const char *path = getenv("MC_FSYNC_TRACE_PATH");
    if (path && *path) output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    final_path = getenv("MC_PROC_FINAL_PATH");
    if (final_path && *final_path) (void)atexit(final_usage);
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
    if (len > 0 && (size_t)len < sizeof(row)) {
        ssize_t recorded = write(output, row, (size_t)len);
        (void)recorded; /* Caller verifies trace completeness against save spans. */
    }
    errno = saved_errno;
    return rc;
}
