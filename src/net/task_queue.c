#include "mc_task_queue.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t queue_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int mc_task_queue_init(mc_task_queue_t *q) {
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
    q->max_len = MC_TASK_QUEUE_DEFAULT_MAX;
    const char *perf = getenv("MC_PERF");
    q->profile = perf && *perf && strcmp(perf, "0") != 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0) return -1;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&q->lock);
        return -1;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&q->lock);
        return -1;
    }
    if (pthread_cond_init(&q->cv, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&q->lock);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    return 0;
}

void mc_task_queue_destroy(mc_task_queue_t *q) {
    if (!q) return;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cv);
}

bool mc_task_queue_push(mc_task_queue_t *q, mc_task_t *task) {
    if (!q || !task) return false;
    task->next = NULL;
    int64_t start_us = q->profile ? queue_now_us() : 0;
    pthread_mutex_lock(&q->lock);
    if (q->profile) {
        q->stats.push_wait_us += (uint64_t)(queue_now_us() - start_us);
        start_us = queue_now_us();
    }
    if (q->max_len > 0 && q->len >= q->max_len) {
        if (q->profile) {
            q->stats.rejected++;
            q->stats.push_hold_us += (uint64_t)(queue_now_us() - start_us);
        }
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    if (q->tail) {
        q->tail->next = task;
    } else {
        q->head = task;
    }
    q->tail = task;
    q->len++;
    if (q->profile) {
        q->stats.pushes++;
        if (q->len > q->stats.peak_depth) q->stats.peak_depth = q->len;
    }
    pthread_cond_signal(&q->cv);
    if (q->profile) q->stats.push_hold_us += (uint64_t)(queue_now_us() - start_us);
    pthread_mutex_unlock(&q->lock);
    return true;
}

mc_task_t *mc_task_queue_drain(mc_task_queue_t *q) {
    if (!q) return NULL;
    int64_t start_us = q->profile ? queue_now_us() : 0;
    pthread_mutex_lock(&q->lock);
    if (q->profile) {
        q->stats.drain_wait_us += (uint64_t)(queue_now_us() - start_us);
        start_us = queue_now_us();
        q->stats.drains++;
    }
    mc_task_t *list = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->len = 0;
    if (q->profile) q->stats.drain_hold_us += (uint64_t)(queue_now_us() - start_us);
    pthread_mutex_unlock(&q->lock);
    return list;
}

void mc_task_queue_get_stats(mc_task_queue_t *q, mc_task_queue_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    *out = q->stats;
    out->depth = q->len;
    pthread_mutex_unlock(&q->lock);
}

bool mc_task_queue_wait_until(mc_task_queue_t *q, int64_t deadline_us) {
    if (!q) return false;

    struct timespec deadline;
    deadline.tv_sec = deadline_us / 1000000;
    deadline.tv_nsec = (deadline_us % 1000000) * 1000;

    pthread_mutex_lock(&q->lock);
    while (!q->head) {
        int rc = pthread_cond_timedwait(&q->cv, &q->lock, &deadline);
        if (rc == ETIMEDOUT) break;
        if (rc != 0) break;
    }
    bool has_tasks = q->head != NULL;
    pthread_mutex_unlock(&q->lock);
    return has_tasks;
}
