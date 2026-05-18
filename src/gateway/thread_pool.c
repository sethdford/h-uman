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
    size_t busy_count;
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
        pool->busy_count++;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        item.fn(item.arg);

        pthread_mutex_lock(&pool->mutex);
        pool->busy_count--;
        pthread_mutex_unlock(&pool->mutex);
    }
}

hu_thread_pool_t *hu_thread_pool_create(size_t n) {
    if (n == 0)
        return NULL;
    /* no allocator in scope — raw malloc */
    hu_thread_pool_t *pool = (hu_thread_pool_t *)malloc(sizeof(hu_thread_pool_t));
    if (!pool)
        return NULL;
    memset(pool, 0, sizeof(*pool));
    pool->thread_count = n;
    pool->threads = (pthread_t *)malloc(n * sizeof(pthread_t)); /* no allocator in scope */
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
    /* F1 (2026-05-18) — C6 ASan fix.
     *
     * Worker threads handle HTTP requests including the chat-completion
     * path, which calls into hu_agent_turn. agent_turn has a HUGE stack
     * frame — variable scope tracking puts locals at offsets 500KB+ from
     * the frame base (deeply nested error-handler scopes in the validator
     * chain + response_guard retry paths). Default pthread stack on macOS
     * is 512KB, which is JUST barely enough; under ASan red zones it
     * overflows and the memset of a topmost local hits the stack guard
     * page → BUS on unknown address.
     *
     * Set 4 MB explicitly. That's the same order of magnitude as the
     * main thread's default stack (8 MB on macOS / Linux glibc) and
     * gives headroom for:
     *   - agent_turn's ~507 KB of locals
     *   - ASan red zones (≈2x overhead)
     *   - Recursive guard retry chains
     *   - The DPO/eval helpers that may inline-call into agent_turn
     *
     * On Linux glibc the default is 8 MB so this is a no-op there in
     * practice, but the explicit setting documents the intent and
     * keeps macOS + Linux behavior aligned.
     *
     * Reproduces with: ASAN_OPTIONS=halt_on_error=1 ./build/human
     *   service-loop --with-gateway, then curl /v1/chat/completions.
     * Pinned by tests/test_thread_pool_worker_stack_size.c. */
    pthread_attr_t attr;
    pthread_attr_t *attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        size_t want = (size_t)4 * 1024 * 1024; /* 4 MB per worker */
        /* setstacksize is best-effort; if it fails (e.g. system policy
         * caps the value) we fall back to the default rather than
         * refuse to create the pool. */
        (void)pthread_attr_setstacksize(&attr, want);
        attr_ptr = &attr;
    }
    for (size_t i = 0; i < n; i++) {
        if (pthread_create(&pool->threads[i], attr_ptr, worker, pool) != 0) {
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->not_empty);
            for (size_t j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->threads);
            free(pool);
            if (attr_ptr)
                pthread_attr_destroy(attr_ptr);
            return NULL;
        }
    }
    if (attr_ptr)
        pthread_attr_destroy(attr_ptr);
    return pool;
}

size_t hu_thread_pool_active(hu_thread_pool_t *pool) {
    if (!pool)
        return 0;
    pthread_mutex_lock(&pool->mutex);
    size_t n = pool->busy_count;
    pthread_mutex_unlock(&pool->mutex);
    return n;
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
