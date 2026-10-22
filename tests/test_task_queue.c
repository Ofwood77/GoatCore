#include "mc_task_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    assert(setenv("MC_PERF", "1", 1) == 0);
    mc_task_queue_t queue;
    assert(mc_task_queue_init(&queue) == 0);
    queue.max_len = 2;
    mc_task_t first = {.packet_id = 1};
    mc_task_t second = {.packet_id = 2};
    mc_task_t rejected = {.packet_id = 3};
    assert(mc_task_queue_push(&queue, &first));
    assert(mc_task_queue_push(&queue, &second));
    assert(!mc_task_queue_push(&queue, &rejected));
    assert(queue.len == 2 && rejected.next == NULL);
    mc_task_t *drained = mc_task_queue_drain(&queue);
    assert(drained == &first && drained->next == &second && second.next == NULL);
    assert(queue.len == 0);
    assert(mc_task_queue_push(&queue, &rejected));
    assert(mc_task_queue_drain(&queue) == &rejected);
    mc_task_queue_stats_t stats;
    mc_task_queue_get_stats(&queue, &stats);
    assert(stats.pushes == 3 && stats.rejected == 1 && stats.drains == 2);
    assert(stats.peak_depth == 2 && stats.depth == 0);
    mc_task_queue_destroy(&queue);
    puts("test_task_queue: ok");
    return 0;
}
