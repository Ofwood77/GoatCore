#include "mc_task_queue.h"
#include <errno.h>
#include <string.h>
#include <time.h>

int mc_task_queue_init(mc_task_queue_t *q) {
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
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

void mc_task_queue_push(mc_task_queue_t *q, mc_task_t *task) {
    if (!q || !task) return;
    task->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->next = task;
    } else {
        q->head = task;
    }
    q->tail = task;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->lock);
}

mc_task_t *mc_task_queue_drain(mc_task_queue_t *q) {
    if (!q) return NULL;
    pthread_mutex_lock(&q->lock);
    mc_task_t *list = q->head;
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_unlock(&q->lock);
    return list;
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
