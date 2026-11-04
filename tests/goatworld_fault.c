/* Test-only process barrier. Absent from normal tool and mc_server. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int entered;
void g1_test_hook(const char *phase) {
    const char *wanted = getenv("G1_TEST_PHASE");
    if (entered || !wanted || strcmp(wanted, phase))
        return;
    entered = 1;
    const char *a = getenv("G1_TEST_READY"), *b = getenv("G1_TEST_RELEASE");
    if (a && b) {
        int ready = atoi(a), release = atoi(b);
        char byte = 'R';
        ssize_t n;
        if (ready < 3 || release < 3 || ready == release)
            _exit(120);
        do {
            n = write(ready, &byte, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 1)
            _exit(121);
        do {
            n = read(release, &byte, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 1)
            _exit(122);
    }
    if (setenv("G1_FAULT_ACTIVE", "1", 1))
        _exit(123);
}
