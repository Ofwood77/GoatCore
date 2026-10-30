#ifndef MC_ATOMIC_FILE_H
#define MC_ATOMIC_FILE_H

#include <stddef.h>
#include <sys/types.h>

/* Only zero acknowledges file fsync + rename + parent fsync. A negative result
 * must leave the caller's dirty state set. Never roll back an uncertain rename. */
#define MC_SAVE_DURABILITY_UNCERTAIN (-2)

typedef struct {
    int dir_fd;
    int temp_fd;
    const char *path;
    char target[256];
    char temporary[256];
} mc_atomic_file_t;

int mc_atomic_file_begin(mc_atomic_file_t *file, const char *path);
int mc_atomic_file_commit(mc_atomic_file_t *file);
void mc_atomic_file_abort(mc_atomic_file_t *file);
int mc_file_write_all(int fd, const void *data, size_t size);
int mc_file_pwrite_all(int fd, const void *data, size_t size, off_t offset);
int mc_file_pread_all(int fd, void *data, size_t size, off_t offset);

/* Persistent advisory lock: never unlink it. Cooperating region writers must
 * hold this descriptor across reading the old region and publishing its copy. */
int mc_atomic_file_lock(const char *path);
int mc_persistence_mkdirs(const char *path, mode_t mode);

#endif
