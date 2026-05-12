#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/plugin_discovery.h"

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

static void test_get_default_dir(void) {
    char buf[512];
    size_t n = hu_plugin_get_default_dir(buf, sizeof(buf));
    /* HOME is set in most environments */
    if (n > 0) {
        HU_ASSERT_STR_CONTAINS(buf, ".human/plugins");
    }
}

static void test_get_default_dir_small_buffer(void) {
    char buf[4];
    size_t n = hu_plugin_get_default_dir(buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0);
}

static void test_discover_null_args(void) {
    hu_plugin_discovery_result_t *res = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_plugin_discover_and_load(NULL, NULL, NULL, &res, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_plugin_discover_and_load(&s_alloc, NULL, NULL, NULL, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_plugin_discover_and_load(&s_alloc, NULL, NULL, &res, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_discover_empty_dir(void) {
    hu_plugin_discovery_result_t *res = NULL;
    size_t count = 0;
    hu_error_t err = hu_plugin_discover_and_load(&s_alloc, "", NULL, &res, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(res);
}

static void test_discover_with_dir(void) {
    hu_plugin_discovery_result_t *res = NULL;
    size_t count = 0;
    hu_error_t err = hu_plugin_discover_and_load(&s_alloc, "/tmp/test-plugins", NULL, &res, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_NOT_NULL(res);
    HU_ASSERT_STR_EQ(res[0].name, "mock-plugin");
    HU_ASSERT_STR_EQ(res[0].version, "1.0.0");
    HU_ASSERT_EQ(res[0].load_error, HU_OK);
    hu_plugin_discovery_results_free(&s_alloc, res, count);
}

static void test_results_free_null(void) {
    hu_plugin_discovery_results_free(&s_alloc, NULL, 0);
    hu_plugin_discovery_results_free(NULL, NULL, 0);
}

static void test_discover_mock_path(void) {
    hu_plugin_discovery_result_t *res = NULL;
    size_t count = 0;
    hu_error_t err = hu_plugin_discover_and_load(&s_alloc, "/fake/dir", NULL, &res, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_NOT_NULL(res[0].path);
    HU_ASSERT_STR_EQ(res[0].path, "mock-plugin.so");
    hu_plugin_discovery_results_free(&s_alloc, res, count);
}

void run_plugin_discovery_tests(void) {
    HU_TEST_SUITE("plugin_discovery");
    HU_RUN_TEST(test_get_default_dir);
    HU_RUN_TEST(test_get_default_dir_small_buffer);
    HU_RUN_TEST(test_discover_null_args);
    HU_RUN_TEST(test_discover_empty_dir);
    HU_RUN_TEST(test_discover_with_dir);
    HU_RUN_TEST(test_results_free_null);
    HU_RUN_TEST(test_discover_mock_path);
}
