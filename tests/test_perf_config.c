#include "mc_util.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

static void *read_config(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        assert(mc_perf_enabled());
        assert(mc_perf_slow_us() == 17000);
        assert(mc_perf_summary_ticks() == 37);
    }
    return NULL;
}

static void *write_trace(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        errno = ENOSPC;
        mc_save_clock_t span = mc_save_profile_begin();
        assert(span.wall_us > 0 && errno == ENOSPC);
        mc_save_profile_end("test", -33, 32, span, 4096, -1);
        assert(errno == ENOSPC);
    }
    return NULL;
}

static void test_save_trace(void) {
    char path[] = "/tmp/mc_save_traceXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0); close(fd);
    pid_t pid = fork(); assert(pid >= 0);
    if (pid == 0) {
        assert(setenv("MC_SAVE_TRACE_PATH", path, 1) == 0);
        pthread_t threads[4];
        for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, write_trace, NULL) == 0);
        for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
        exit(0); /* Flush diagnostic stream just as normal server shutdown does. */
    }
    int status = 0; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    FILE *file = fopen(path, "r"); assert(file);
    int lines = 0; char line[512];
    while (fgets(line, sizeof(line), file)) lines++;
    assert(lines == 401); fclose(file); unlink(path);
    assert(unsetenv("MC_SAVE_TRACE_PATH") == 0);
    errno = EIO;
    mc_save_clock_t disabled = mc_save_profile_begin();
    assert(disabled.wall_us == 0 && disabled.cpu_us == 0 && errno == EIO);
    mc_save_profile_end("disabled", 0, 0, disabled, 0, 0);
    assert(errno == EIO);
}

int main(void) {
    test_save_trace();
    assert(setenv("MC_PERF", "1", 1) == 0);
    assert(setenv("MC_PERF_SLOW_MS", "17", 1) == 0);
    assert(setenv("MC_PERF_SUMMARY_TICKS", "37", 1) == 0);
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++) assert(pthread_create(&threads[i], NULL, read_config, NULL) == 0);
    for (size_t i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
    puts("test_perf_config: ok");
    return 0;
}
