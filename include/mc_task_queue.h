#ifndef MC_TASK_QUEUE_H
#define MC_TASK_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "mc_net.h"

#define MC_TASK_QUEUE_DEFAULT_MAX 8192u

typedef enum {
    MC_TASK_PACKET = 0
} mc_task_type_t;

typedef struct mc_task {
    mc_task_type_t type;
    mc_conn_t *conn;
    int32_t packet_id;
    uint8_t *payload;
    size_t payload_len;
    int64_t enqueue_ms;
    struct mc_task *next;
} mc_task_t;

typedef struct {
    mc_task_t *head;
    mc_task_t *tail;
    size_t len;
    size_t max_len;
    pthread_mutex_t lock;
    pthread_cond_t cv;
} mc_task_queue_t;

int mc_task_queue_init(mc_task_queue_t *q);
void mc_task_queue_destroy(mc_task_queue_t *q);
bool mc_task_queue_push(mc_task_queue_t *q, mc_task_t *task);
mc_task_t *mc_task_queue_drain(mc_task_queue_t *q);
bool mc_task_queue_wait_until(mc_task_queue_t *q, int64_t deadline_us);

#endif /* MC_TASK_QUEUE_H */
