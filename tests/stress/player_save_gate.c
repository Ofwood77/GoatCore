/* Test-only LD_PRELOAD: one bounded pause after a player temporary fsync.
 * Never load in a main service. No hook is linked into mc_server. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_bool used;
int fsync(int fd) {
    int (*real_sync)(int) = dlsym(RTLD_NEXT, "fsync");
    if (!real_sync) { errno = EIO; return -1; }
    int result = real_sync(fd), saved = errno;
    const char *root = getenv("MC_PLAYER_GATE_DIR");
    char link[64], target[4096], path[4096];
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t length = readlink(link,target,sizeof(target)-1);
    if (!result && root && length>0) {
        target[length]=0;
        snprintf(path,sizeof(path),"%s/arm",root);
        if (strstr(target,"/players/data/.player-") && access(path,F_OK)==0 && !atomic_exchange(&used,1)) {
            snprintf(path,sizeof(path),"%s/ready",root);
            int marker=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
            if (marker<0) return -1;
            close(marker);
            snprintf(path,sizeof(path),"%s/release",root);
            for (int i=0;i<1500;i++) {
                if (access(path,F_OK)==0) { errno=saved; return result; }
                struct timespec delay={0,10000000};nanosleep(&delay,NULL);
            }
            errno=ETIMEDOUT;return -1;
        }
    }
    errno=saved;return result;
}
