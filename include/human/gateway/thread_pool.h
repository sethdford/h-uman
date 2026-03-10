#ifndef HU_THREAD_POOL_H
#define HU_THREAD_POOL_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*hu_work_fn)(void *arg);

typedef struct hu_thread_pool hu_thread_pool_t;

/* Create a pool with `n` worker threads */
hu_thread_pool_t *hu_thread_pool_create(size_t n);

/* Submit work. Returns false if the queue is full or pool is shutting down. */
bool hu_thread_pool_submit(hu_thread_pool_t *pool, hu_work_fn fn, void *arg);

/* Drain pending work and join all threads. */
void hu_thread_pool_destroy(hu_thread_pool_t *pool);

#endif /* HU_THREAD_POOL_H */
