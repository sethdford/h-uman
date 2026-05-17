#include "test_framework.h"
#include "human/agent/approval_gate.h"
#include "human/core/allocator.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Allocator wrapper functions */
static void *test_alloc_wrapper(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void test_free_wrapper(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

/* Test allocator */
static hu_allocator_t test_alloc = {
    .ctx = NULL,
    .alloc = test_alloc_wrapper,
    .free = test_free_wrapper,
    .realloc = NULL
};

/* Return a per-pid, per-tag tmp path so cross-run state can't leak.
 * Walks the directory and unlinks every entry before returning so the
 * test starts with a guaranteed-empty directory. Two binaries running
 * concurrently use different pids → no collision. */
static const char *gates_path(int tag) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "/tmp/hu_test_gates_%d_%d", (int)getpid(), tag);
    DIR *d = opendir(buf);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", buf, de->d_name);
            unlink(path);
        }
        closedir(d);
    }
    return buf;
}

static void test_gate_manager_create(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_error_t err = hu_gate_manager_create(&test_alloc, gates_path(1), &mgr);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(mgr);

    const char *dir = hu_gate_manager_dir(mgr);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_STR_CONTAINS(dir, "gate");

    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_create_and_check(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(2), &mgr);
    HU_ASSERT_NOT_NULL(mgr);

    char gate_id[64];
    const char *description = "Approve deployment to production";
    const char *context = "{\"env\":\"prod\",\"version\":\"1.2.3\"}";

    hu_error_t err = hu_gate_create(mgr, &test_alloc, description, strlen(description), context,
                                    strlen(context), 3600, gate_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NEQ(gate_id[0], '\0');

    hu_gate_status_t status;
    err = hu_gate_check(mgr, gate_id, &status);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status, HU_GATE_PENDING);

    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_load_details(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(3), &mgr);

    char gate_id[64];
    const char *desc = "Test approval gate";
    const char *ctx = "{\"key\":\"value\"}";

    hu_gate_create(mgr, &test_alloc, desc, strlen(desc), ctx, strlen(ctx), 0, gate_id);

    hu_approval_gate_t gate;
    hu_error_t err = hu_gate_load(mgr, &test_alloc, gate_id, &gate);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(gate.gate_id, gate_id);
    HU_ASSERT_STR_EQ(gate.description, desc);
    HU_ASSERT_EQ(gate.status, HU_GATE_PENDING);
    HU_ASSERT_EQ(gate.timeout_at, 0);

    hu_gate_free(&test_alloc, &gate);
    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_resolve_approve(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(4), &mgr);

    char gate_id[64];
    hu_gate_create(mgr, &test_alloc, "Approve?", 8, NULL, 0, 0, gate_id);

    const char *response = "Approved by user123";
    hu_error_t err = hu_gate_resolve(mgr, &test_alloc, gate_id, HU_GATE_APPROVED, response,
                                     strlen(response));
    HU_ASSERT_EQ(err, HU_OK);

    hu_gate_status_t status;
    err = hu_gate_check(mgr, gate_id, &status);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status, HU_GATE_APPROVED);

    hu_approval_gate_t gate;
    err = hu_gate_load(mgr, &test_alloc, gate_id, &gate);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(gate.response, response);
    HU_ASSERT_GT(gate.resolved_at, 0);

    hu_gate_free(&test_alloc, &gate);
    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_resolve_reject(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(5), &mgr);

    char gate_id[64];
    hu_gate_create(mgr, &test_alloc, "Reject test?", 12, NULL, 0, 0, gate_id);

    const char *reason = "Not ready for deployment";
    hu_error_t err = hu_gate_resolve(mgr, &test_alloc, gate_id, HU_GATE_REJECTED, reason,
                                     strlen(reason));
    HU_ASSERT_EQ(err, HU_OK);

    hu_gate_status_t status;
    err = hu_gate_check(mgr, gate_id, &status);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status, HU_GATE_REJECTED);

    hu_approval_gate_t gate;
    err = hu_gate_load(mgr, &test_alloc, gate_id, &gate);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(gate.response, reason);

    hu_gate_free(&test_alloc, &gate);
    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_timeout(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(6), &mgr);

    char gate_id[64];
    /* Create gate with 1-second timeout */
    hu_gate_create(mgr, &test_alloc, "Will timeout", 12, NULL, 0, 1, gate_id);

    hu_gate_status_t status;
    hu_error_t err = hu_gate_check(mgr, gate_id, &status);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status, HU_GATE_PENDING);

    /* Wait for timeout */
#ifndef HU_IS_TEST
    sleep(2);
#else
    usleep(2000000);
#endif

    /* Check again — should be timed out */
    err = hu_gate_check(mgr, gate_id, &status);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status, HU_GATE_TIMED_OUT);

    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_list_pending_empty(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(7), &mgr);

    hu_approval_gate_t *gates = NULL;
    size_t count = 0;
    hu_error_t err = hu_gate_list_pending(mgr, &test_alloc, &gates, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(gates);

    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_list_pending_multiple(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(8), &mgr);

    /* Create 3 pending gates */
    char gate_id1[64], gate_id2[64], gate_id3[64];
    hu_gate_create(mgr, &test_alloc, "Gate 1", 6, NULL, 0, 0, gate_id1);
    hu_gate_create(mgr, &test_alloc, "Gate 2", 6, NULL, 0, 0, gate_id2);
    hu_gate_create(mgr, &test_alloc, "Gate 3", 6, NULL, 0, 0, gate_id3);

    /* Resolve one to approved (should not appear in pending list) */
    hu_gate_resolve(mgr, &test_alloc, gate_id1, HU_GATE_APPROVED, "OK", 2);

    hu_approval_gate_t *gates = NULL;
    size_t count = 0;
    hu_error_t err = hu_gate_list_pending(mgr, &test_alloc, &gates, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2); /* Only gates 2 and 3 are pending */

    hu_gate_free_array(&test_alloc, gates, count);
    hu_gate_manager_destroy(mgr, &test_alloc);
}

static void test_gate_persistence(void) {
    const char *gate_id_save = NULL;
    const char *persist_dir = gates_path(9); /* Reused across the two manager open/close cycles */

    /* Create and save a gate */
    {
        hu_gate_manager_t *mgr = NULL;
        hu_gate_manager_create(&test_alloc, persist_dir, &mgr);

        char gate_id[64];
        hu_gate_create(mgr, &test_alloc, "Persist test", 12, NULL, 0, 0, gate_id);
        gate_id_save = (const char *)malloc(strlen(gate_id) + 1);
        strcpy((char *)gate_id_save, gate_id);

        hu_gate_manager_destroy(mgr, &test_alloc);
    }

    /* Recreate manager and verify gate is still there. Pass the same
     * persist_dir — `gates_path` would have cleaned it on a second
     * call, which would defeat the persistence assertion. */
    {
        hu_gate_manager_t *mgr = NULL;
        hu_gate_manager_create(&test_alloc, persist_dir, &mgr);

        hu_gate_status_t status;
        hu_error_t err = hu_gate_check(mgr, gate_id_save, &status);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_EQ(status, HU_GATE_PENDING);

        hu_gate_manager_destroy(mgr, &test_alloc);
    }

    free((void *)gate_id_save);
}

static void test_gate_status_names(void) {
    HU_ASSERT_STR_EQ(hu_gate_status_name(HU_GATE_PENDING), "pending");
    HU_ASSERT_STR_EQ(hu_gate_status_name(HU_GATE_APPROVED), "approved");
    HU_ASSERT_STR_EQ(hu_gate_status_name(HU_GATE_REJECTED), "rejected");
    HU_ASSERT_STR_EQ(hu_gate_status_name(HU_GATE_TIMED_OUT), "timed_out");
}

static void test_gate_memory_cleanup(void) {
    hu_gate_manager_t *mgr = NULL;
    hu_gate_manager_create(&test_alloc, gates_path(10), &mgr);

    char gate_id[64];
    hu_gate_create(mgr, &test_alloc, "Cleanup test", 12, NULL, 0, 0, gate_id);

    hu_approval_gate_t gate;
    hu_gate_load(mgr, &test_alloc, gate_id, &gate);

    /* Verify cleanup doesn't crash */
    hu_gate_free(&test_alloc, &gate);

    hu_gate_manager_destroy(mgr, &test_alloc);
}

void run_approval_gate_tests(void) {
    HU_TEST_SUITE("approval_gate");

    HU_RUN_TEST(test_gate_manager_create);
    HU_RUN_TEST(test_gate_create_and_check);
    HU_RUN_TEST(test_gate_load_details);
    HU_RUN_TEST(test_gate_resolve_approve);
    HU_RUN_TEST(test_gate_resolve_reject);
    HU_RUN_TEST(test_gate_timeout);
    HU_RUN_TEST(test_gate_list_pending_empty);
    HU_RUN_TEST(test_gate_list_pending_multiple);
    HU_RUN_TEST(test_gate_persistence);
    HU_RUN_TEST(test_gate_status_names);
    HU_RUN_TEST(test_gate_memory_cleanup);
}
