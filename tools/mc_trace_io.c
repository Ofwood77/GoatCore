/* Child-only Linux syscall probe. Never attach to an existing service.
 * Counts/crash experiments only: ptrace perturbs timings. */
#define _GNU_SOURCE
#if !defined(__linux__) || !defined(__x86_64__)
#error "mc_trace_io requires Linux x86_64 (fault injection uses the syscall argument registers)"
#endif
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
    pid_t tid;
    long nr, fd;
    uint64_t bytes;
    int flags;
    int64_t start;
    char path[2048];
    int live;
} task_t;
static task_t tasks[256];
static int64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static task_t *task(pid_t pid) {
    for (int i = 0; i < 256; i++)
        if (tasks[i].live && tasks[i].tid == pid)
            return &tasks[i];
    for (int i = 0; i < 256; i++)
        if (!tasks[i].live) {
            memset(&tasks[i], 0, sizeof(tasks[i]));
            tasks[i].live = 1;
            tasks[i].tid = pid;
            return &tasks[i];
        }
    abort();
}
static void string_at(pid_t pid, unsigned long address, char *out, size_t cap) {
    size_t pos = 0;
    while (pos + sizeof(long) < cap) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, address + pos, 0);
        if (errno)
            break;
        memcpy(out + pos, &word, sizeof(word));
        for (size_t j = 0; j < sizeof(word); j++)
            if (!out[pos + j])
                return;
        pos += sizeof(word);
    }
    out[pos] = 0;
}
static const char *name(long n) {
    switch (n) {
    case SYS_openat:
        return "openat";
    case SYS_read:
        return "read";
    case SYS_write:
        return "write";
    case SYS_pread64:
        return "pread";
    case SYS_pwrite64:
        return "pwrite";
    case SYS_writev:
        return "writev";
    case SYS_fsync:
        return "fsync";
    case SYS_fdatasync:
        return "fdatasync";
    case SYS_close:
        return "close";
    case SYS_ftruncate:
        return "ftruncate";
    case SYS_lseek:
        return "lseek";
    default:
        return NULL;
    }
}
int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s OUT MATCH TARGET KILL_STAGE -- PROGRAM [ARGS...]\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[5], "--"))
        return 2;
    const char *stages[] = {"none",          "open-before", "open-truncate", "write-before",
                            "write-partial", "write-after", "close-after"};
    int valid_stage = 0;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
        if (!strcmp(argv[4], stages[i]))
            valid_stage = 1;
    if (!valid_stage || (strcmp(argv[4], "none") && !argv[3][0]))
        return 2;
    FILE *out = fopen(argv[1], "w");
    if (!out) {
        perror("trace output");
        return 2;
    }
    setvbuf(out, NULL, _IOFBF, 65536);
    fprintf(out, "thread,start_us,end_us,call,fd,requested,result,path\n");
    fflush(out);
    const char *match = argv[2], *target = argv[3], *stage = argv[4];
    pid_t leader = fork();
    if (leader < 0)
        return 2;
    if (!leader) {
        fclose(out);
        struct rlimit lim = {0, 0};
        setrlimit(RLIMIT_CORE, &lim);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
            perror("ptrace");
            _exit(126);
        }
        raise(SIGSTOP);
        execvp(argv[6], argv + 6);
        perror("exec");
        _exit(127);
    }
    int status;
    if (waitpid(leader, &status, 0) < 0 || !WIFSTOPPED(status))
        return 2;
    if (ptrace(PTRACE_SETOPTIONS, leader, 0,
               PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL) < 0) {
        perror("options");
        kill(leader, SIGKILL);
        return 2;
    }
    task(leader);
    ptrace(PTRACE_SYSCALL, leader, 0, 0);
    int code = 0, killed = 0;
    pid_t partial_tid = 0;
    for (;;) {
        pid_t pid = waitpid(-1, &status, __WALL);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        task_t *t = task(pid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (pid == leader)
                code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            t->live = 0;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;
        int sig = WSTOPSIG(status), deliver = 0;
        if ((status >> 16) == PTRACE_EVENT_CLONE) {
            unsigned long child = 0;
            ptrace(PTRACE_GETEVENTMSG, pid, 0, &child);
            task((pid_t)child);
        } else if (sig == (SIGTRAP | 0x80)) {
            struct ptrace_syscall_info info;
            memset(&info, 0, sizeof(info));
            if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), &info) < 0) {
                perror("syscall info");
                kill(leader, SIGKILL);
                code = 2;
                break;
            }
            if (info.op == PTRACE_SYSCALL_INFO_ENTRY) {
                t->nr = (long)info.entry.nr;
                t->path[0] = 0;
                t->fd = (long)info.entry.args[0];
                t->bytes = info.entry.args[2];
                t->flags = 0;
                t->start = now();
                if (t->nr == SYS_openat) {
                    string_at(pid, (unsigned long)info.entry.args[1], t->path, sizeof(t->path));
                    t->flags = (int)info.entry.args[2];
                    t->bytes = 0;
                } else if (name(t->nr)) {
                    char fdpath[80];
                    snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd/%ld", pid, t->fd);
                    ssize_t n = readlink(fdpath, t->path, sizeof(t->path) - 1);
                    if (n >= 0)
                        t->path[n] = 0;
                    else
                        t->path[0] = 0;
                }
                if (!killed && target[0] && !strcmp(t->path, target) && !strcmp(stage, "open-before") &&
                    t->nr == SYS_openat && (t->flags & O_TRUNC)) {
                    kill(leader, SIGKILL);
                    killed = 1;
                }
                if (!killed && target[0] && strcmp(stage, "none") && !strcmp(t->path, target) &&
                    (t->nr == SYS_write || t->nr == SYS_pwrite64)) {
                    if (!strcmp(stage, "write-before")) {
                        kill(leader, SIGKILL);
                        killed = 1;
                    } else if (!strcmp(stage, "write-partial")) {
                        struct user_regs_struct regs;
                        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0)
                            abort();
                        regs.rdx = regs.rdx > 1 ? regs.rdx / 2 : 1;
                        if (ptrace(PTRACE_SETREGS, pid, 0, &regs) < 0)
                            abort();
                        partial_tid = pid;
                    }
                }
            } else if (info.op == PTRACE_SYSCALL_INFO_EXIT) {
                const char *call = name(t->nr);
                int relevant = call && strstr(t->path, match);
                if (relevant)
                    fprintf(out, "%d,%lld,%lld,%s,%ld,%llu,%lld,%s\n", pid, (long long)t->start,
                            (long long)now(), call, t->fd, (unsigned long long)t->bytes,
                            (long long)info.exit.rval, t->path);
                if (!killed && target[0] && !strcmp(t->path, target)) {
                    int stop = (!strcmp(stage, "open-truncate") && t->nr == SYS_openat &&
                                (t->flags & O_TRUNC) && info.exit.rval >= 0) ||
                               (!strcmp(stage, "write-after") &&
                                (t->nr == SYS_write || t->nr == SYS_pwrite64) && info.exit.rval > 0) ||
                               (!strcmp(stage, "write-partial") && partial_tid == pid) ||
                               (!strcmp(stage, "close-after") && t->nr == SYS_close && info.exit.rval == 0);
                    /* close-after applies only after the writer's truncate, not its initial read. */
                    static int writer_open = 0;
                    if (t->nr == SYS_openat && (t->flags & O_TRUNC) && info.exit.rval >= 0)
                        writer_open = 1;
                    if (!strcmp(stage, "close-after") && !writer_open)
                        stop = 0;
                    if (stop) {
                        kill(leader, SIGKILL);
                        killed = 1;
                    }
                }
                t->nr = -1;
                t->path[0] = 0;
            }
        } else if (sig != SIGTRAP && sig != SIGSTOP)
            deliver = sig;
        if (ptrace(PTRACE_SYSCALL, pid, 0, deliver) < 0 && errno != ESRCH) {
            perror("resume");
            kill(leader, SIGKILL);
            code = 2;
            break;
        }
    }
    if (fclose(out) != 0) {
        perror("trace close");
        return 2;
    }
    if (strcmp(stage, "none") && !killed) {
        fprintf(stderr, "requested crash point not reached\n");
        return 3;
    }
    return code;
}
