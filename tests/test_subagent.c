#include "human/config.h"
#include "human/core/allocator.h"
#include "human/subagent.h"
#include "test_framework.h"
#include <string.h>

static void test_subagent_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(mgr);
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_spawn_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(mgr);
    uint64_t task_id = 0;
    hu_error_t err = hu_subagent_spawn(mgr, "test task", 9, "test", NULL, NULL, &task_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(task_id > 0);
    /* In test mode, task completes synchronously */
    hu_task_status_t status = hu_subagent_get_status(mgr, task_id);
    HU_ASSERT_EQ(status, HU_TASK_COMPLETED);
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_get_result(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    uint64_t task_id = 0;
    hu_subagent_spawn(mgr, "hello", 5, "greet", NULL, NULL, &task_id);
    const char *result = hu_subagent_get_result(mgr, task_id);
    HU_ASSERT_NOT_NULL(result);
    HU_ASSERT_STR_EQ(result, "completed: hello");
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_running_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    /* In test mode all complete immediately so running count stays 0 */
    size_t count = hu_subagent_running_count(mgr);
    HU_ASSERT_EQ(count, 0u);
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_invalid_task_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    hu_task_status_t status = hu_subagent_get_status(mgr, 999);
    HU_ASSERT_EQ(status, HU_TASK_FAILED); /* not found */
    const char *result = hu_subagent_get_result(mgr, 999);
    HU_ASSERT(result == NULL);
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_cancel_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    hu_error_t err = hu_subagent_cancel(mgr, 999);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_subagent_destroy(&alloc, mgr);
}

static void test_subagent_get_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_subagent_manager_t *mgr = hu_subagent_create(&alloc, NULL);
    uint64_t id1 = 0, id2 = 0;
    hu_subagent_spawn(mgr, "a", 1, "l1", NULL, NULL, &id1);
    hu_subagent_spawn(mgr, "b", 1, "l2", NULL, NULL, &id2);
    hu_subagent_task_info_t *infos = NULL;
    size_t count = 0;
    hu_error_t err = hu_subagent_get_all(mgr, &alloc, &infos, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(infos);
    HU_ASSERT(infos[0].task_id == id1 || infos[0].task_id == id2);
    HU_ASSERT(infos[1].task_id == id1 || infos[1].task_id == id2);
    alloc.free(alloc.ctx, (void *)infos, count * sizeof(hu_subagent_task_info_t));
    hu_subagent_destroy(&alloc, mgr);
}

void run_subagent_tests(void) {
    HU_TEST_SUITE("Subagent");
    HU_RUN_TEST(test_subagent_create_destroy);
    HU_RUN_TEST(test_subagent_spawn_test_mode);
    HU_RUN_TEST(test_subagent_get_result);
    HU_RUN_TEST(test_subagent_running_count);
    HU_RUN_TEST(test_subagent_invalid_task_id);
    HU_RUN_TEST(test_subagent_cancel_invalid);
    HU_RUN_TEST(test_subagent_get_all);
}
