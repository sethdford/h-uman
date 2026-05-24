#include "human/gateway/thread_pool.h"
#include "test_framework.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static volatile int counter;

static void increment_counter(void *arg) {
    (void)arg;
    __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
}

static void thread_pool_create_null_returns_null(void) {
    HU_ASSERT_NULL(hu_thread_pool_create(0));
}

static void thread_pool_create_and_destroy(void) {
    hu_thread_pool_t *pool = hu_thread_pool_create(2);
    HU_ASSERT_NOT_NULL(pool);
    hu_thread_pool_destroy(pool);
}

static void thread_pool_destroy_null_is_safe(void) {
    hu_thread_pool_destroy(NULL);
}

static void thread_pool_submit_null_pool_returns_false(void) {
    HU_ASSERT_FALSE(hu_thread_pool_submit(NULL, increment_counter, NULL));
}

static void thread_pool_submit_null_fn_returns_false(void) {
    hu_thread_pool_t *pool = hu_thread_pool_create(1);
    HU_ASSERT_NOT_NULL(pool);
    HU_ASSERT_FALSE(hu_thread_pool_submit(pool, NULL, NULL));
    hu_thread_pool_destroy(pool);
}

static void thread_pool_submit_and_execute(void) {
    __atomic_store_n(&counter, 0, __ATOMIC_SEQ_CST);
    hu_thread_pool_t *pool = hu_thread_pool_create(2);
    HU_ASSERT_NOT_NULL(pool);
    for (int i = 0; i < 10; i++)
        HU_ASSERT_TRUE(hu_thread_pool_submit(pool, increment_counter, NULL));
    hu_thread_pool_destroy(pool);
    HU_ASSERT_EQ(__atomic_load_n(&counter, __ATOMIC_SEQ_CST), 10);
}

static void thread_pool_active_null_returns_zero(void) {
    HU_ASSERT_EQ((int)hu_thread_pool_active(NULL), 0);
}

/* F1 (2026-05-18) — C6 ASan regression guard.
 *
 * Worker threads must have at least 2 MB stack. agent_turn's stack
 * frame approaches 512 KB (default macOS pthread stack) and overruns
 * the guard page under ASan. We submit a job that probes the worker's
 * own pthread stack size and report it back through a shared variable.
 *
 * Test passes iff the reported stack size is at least 2 MB. Catches
 * regressions where someone removes the pthread_attr_setstacksize
 * call or sets a value below our 4 MB target. */
static volatile size_t reported_stack_size;

static void probe_stack_size(void *arg) {
    (void)arg;
    pthread_attr_t attr;
    /* pthread_get_stacksize_np is macOS / Linux glibc; portable enough
     * for this test which only runs locally + in CI Linux/macOS. */
    if (pthread_attr_init(&attr) == 0) {
        size_t sz = 0;
#if defined(__APPLE__)
        sz = pthread_get_stacksize_np(pthread_self());
#else
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void *addr;
            pthread_attr_getstack(&attr, &addr, &sz);
        }
#endif
        __atomic_store_n(&reported_stack_size, sz, __ATOMIC_SEQ_CST);
        pthread_attr_destroy(&attr);
    }
}

static void thread_pool_worker_has_4mb_stack(void) {
    __atomic_store_n(&reported_stack_size, 0, __ATOMIC_SEQ_CST);
    hu_thread_pool_t *pool = hu_thread_pool_create(1);
    HU_ASSERT_NOT_NULL(pool);
    HU_ASSERT_TRUE(hu_thread_pool_submit(pool, probe_stack_size, NULL));
    /* Give the worker time to run the probe and report. */
    usleep(200 * 1000);
    hu_thread_pool_destroy(pool);
    size_t sz = __atomic_load_n(&reported_stack_size, __ATOMIC_SEQ_CST);
    /* 4 MB is the documented target. Allow some slack — the assertion
     * is "at least 2 MB" which is the threshold that prevents the
     * agent_turn frame overflow. If pthread_attr_setstacksize failed
     * silently the worker still gets the default ~512 KB and this
     * assertion fires immediately. */
    HU_ASSERT_TRUE(sz >= (size_t)2 * 1024 * 1024);
}

void run_thread_pool_tests(void) {
    HU_TEST_SUITE("ThreadPool");

    HU_RUN_TEST(thread_pool_create_null_returns_null);
    HU_RUN_TEST(thread_pool_create_and_destroy);
    HU_RUN_TEST(thread_pool_destroy_null_is_safe);
    HU_RUN_TEST(thread_pool_submit_null_pool_returns_false);
    HU_RUN_TEST(thread_pool_submit_null_fn_returns_false);
    HU_RUN_TEST(thread_pool_submit_and_execute);
    HU_RUN_TEST(thread_pool_active_null_returns_zero);
    HU_RUN_TEST(thread_pool_worker_has_4mb_stack);
}
