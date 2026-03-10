#include "human/gateway/thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define POOL_QUEUE_SIZE 64

typedef struct {
    hu_work_fn fn;
    void *arg;
} hu_work_item_t;

struct hu_thread_pool {
    pthread_t *threads;
    size_t thread_count;
    hu_work_item_t queue[POOL_QUEUE_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    bool shutdown;
};

static void *worker(void *arg) {
    hu_thread_pool_t *pool = (hu_thread_pool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        hu_work_item_t item = pool->queue[pool->head];
        pool->head = (pool->head + 1) % POOL_QUEUE_SIZE;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);
        item.fn(item.arg);
    }
}

hu_thread_pool_t *hu_thread_pool_create(size_t n) {
    if (n == 0)
        return NULL;
    hu_thread_pool_t *pool = (hu_thread_pool_t *)malloc(sizeof(hu_thread_pool_t));
    if (!pool)
        return NULL;
    memset(pool, 0, sizeof(*pool));
    pool->thread_count = n;
    pool->threads = (pthread_t *)malloc(n * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker, pool) != 0) {
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->not_empty);
            for (size_t j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

bool hu_thread_pool_submit(hu_thread_pool_t *pool, hu_work_fn fn, void *arg) {
    if (!pool || !fn)
        return false;
    pthread_mutex_lock(&pool->mutex);
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    while (pool->count >= POOL_QUEUE_SIZE)
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    pool->queue[pool->tail].fn = fn;
    pool->queue[pool->tail].arg = arg;
    pool->tail = (pool->tail + 1) % POOL_QUEUE_SIZE;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return true;
}

void hu_thread_pool_destroy(hu_thread_pool_t *pool) {
    if (!pool)
        return;
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);
    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    free(pool);
}
