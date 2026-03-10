#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "test_framework.h"
#include <string.h>

static void test_migrate_missing_version_defaults_to_v1(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = hu_json_object_new(&alloc);
    hu_error_t err = hu_config_migrate(&alloc, root);
    HU_ASSERT_EQ(err, HU_OK);
    double ver = hu_json_get_number(root, "config_version", 0);
    HU_ASSERT_EQ((int)ver, HU_CONFIG_VERSION_CURRENT);
    hu_json_free(&alloc, root);
}

static void test_migrate_current_version_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, root, "config_version",
                       hu_json_number_new(&alloc, (double)HU_CONFIG_VERSION_CURRENT));
    hu_error_t err = hu_config_migrate(&alloc, root);
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_free(&alloc, root);
}

static void test_migrate_future_version_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, root, "config_version", hu_json_number_new(&alloc, 999.0));
    hu_error_t err = hu_config_migrate(&alloc, root);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_json_free(&alloc, root);
}

static void test_migrate_v1_memory_backend_moved(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, root, "memory_backend", hu_json_string_new(&alloc, "sqlite", 6));
    hu_error_t err = hu_config_migrate(&alloc, root);
    HU_ASSERT_EQ(err, HU_OK);
    const hu_json_value_t *mem = hu_json_object_get(root, "memory");
    HU_ASSERT_TRUE(mem != NULL);
    if (mem) {
        const char *be = hu_json_get_string(mem, "backend");
        HU_ASSERT_TRUE(be != NULL);
        if (be)
            HU_ASSERT_STR_EQ(be, "sqlite");
    }
    hu_json_free(&alloc, root);
}

void run_config_migrate_tests(void) {
    HU_RUN_TEST(test_migrate_missing_version_defaults_to_v1);
    HU_RUN_TEST(test_migrate_current_version_noop);
    HU_RUN_TEST(test_migrate_future_version_rejected);
    HU_RUN_TEST(test_migrate_v1_memory_backend_moved);
}
