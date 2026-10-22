#include "mc_util.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *read_config(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        assert(mc_perf_enabled());
        assert(mc_perf_slow_us() == 17000);
        assert(mc_perf_summary_ticks() == 37);
    }
    return NULL;
}

int main(void) {
    assert(setenv("MC_PERF", "1", 1) == 0);
    assert(setenv("MC_PERF_SLOW_MS", "17", 1) == 0);
    assert(setenv("MC_PERF_SUMMARY_TICKS", "37", 1) == 0);
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++) assert(pthread_create(&threads[i], NULL, read_config, NULL) == 0);
    for (size_t i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
    puts("test_perf_config: ok");
    return 0;
}
