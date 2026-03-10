#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/gateway/tenant.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static hu_tenant_t make_tenant(const char *id, const char *name, hu_tenant_role_t role,
                               uint32_t rpm, uint64_t quota) {
    hu_tenant_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.user_id, sizeof(t.user_id), "%s", id);
    snprintf(t.display_name, sizeof(t.display_name), "%s", name);
    t.role = role;
    t.rate_limit_rpm = rpm;
    t.usage_quota_tokens = quota;
    return t;
}

static void test_tenant_store_init_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_error_t err = hu_tenant_store_init(&alloc, &store);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(store);
    hu_tenant_store_destroy(store);
}

static void test_tenant_crud(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("user-1", "Alice", HU_TENANT_ROLE_ADMIN, 60, 100000);
    HU_ASSERT_EQ(hu_tenant_create(store, &t), HU_OK);
    hu_tenant_t out = {0};
    HU_ASSERT_EQ(hu_tenant_get(store, "user-1", &out), HU_OK);
    HU_ASSERT_STR_EQ(out.display_name, "Alice");
    HU_ASSERT_EQ((int)out.role, (int)HU_TENANT_ROLE_ADMIN);
    snprintf(t.display_name, sizeof(t.display_name), "Alice Updated");
    HU_ASSERT_EQ(hu_tenant_update(store, &t), HU_OK);
    HU_ASSERT_EQ(hu_tenant_get(store, "user-1", &out), HU_OK);
    HU_ASSERT_STR_EQ(out.display_name, "Alice Updated");
    HU_ASSERT_EQ(hu_tenant_delete(store, "user-1"), HU_OK);
    HU_ASSERT_NEQ(hu_tenant_get(store, "user-1", &out), HU_OK);
    hu_tenant_store_destroy(store);
}

static void test_tenant_duplicate_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("dup", "Bob", HU_TENANT_ROLE_USER, 0, 0);
    HU_ASSERT_EQ(hu_tenant_create(store, &t), HU_OK);
    HU_ASSERT_NEQ(hu_tenant_create(store, &t), HU_OK);
    hu_tenant_store_destroy(store);
}

static void test_tenant_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t ta = make_tenant("a", "A", HU_TENANT_ROLE_USER, 0, 0);
    hu_tenant_t tb = make_tenant("b", "B", HU_TENANT_ROLE_USER, 0, 0);
    HU_ASSERT_EQ(hu_tenant_create(store, &ta), HU_OK);
    HU_ASSERT_EQ(hu_tenant_create(store, &tb), HU_OK);
    hu_tenant_t out[10];
    size_t count = 0;
    HU_ASSERT_EQ(hu_tenant_list(store, out, 10, &count), HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_tenant_store_destroy(store);
}

static void test_tenant_quota_enforcement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("quota", "Q", HU_TENANT_ROLE_USER, 0, 1000);
    hu_tenant_create(store, &t);
    HU_ASSERT_TRUE(hu_tenant_check_quota(store, "quota"));
    hu_tenant_increment_usage(store, "quota", 999);
    HU_ASSERT_TRUE(hu_tenant_check_quota(store, "quota"));
    hu_tenant_increment_usage(store, "quota", 2);
    HU_ASSERT_FALSE(hu_tenant_check_quota(store, "quota"));
    hu_tenant_store_destroy(store);
}

static void test_tenant_rate_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("rl", "RL", HU_TENANT_ROLE_USER, 3, 0);
    hu_tenant_create(store, &t);
    HU_ASSERT_TRUE(hu_tenant_check_rate_limit(store, "rl"));
    HU_ASSERT_TRUE(hu_tenant_check_rate_limit(store, "rl"));
    HU_ASSERT_TRUE(hu_tenant_check_rate_limit(store, "rl"));
    HU_ASSERT_FALSE(hu_tenant_check_rate_limit(store, "rl"));
    hu_tenant_store_destroy(store);
}

static void test_tenant_unlimited_quota(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("unl", "U", HU_TENANT_ROLE_ADMIN, 0, 0);
    hu_tenant_create(store, &t);
    HU_ASSERT_TRUE(hu_tenant_check_quota(store, "unl"));
    HU_ASSERT_TRUE(hu_tenant_check_rate_limit(store, "unl"));
    hu_tenant_increment_usage(store, "unl", 999999);
    HU_ASSERT_TRUE(hu_tenant_check_quota(store, "unl"));
    hu_tenant_store_destroy(store);
}

static void test_tenant_readonly_role(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t t = make_tenant("ro", "RO", HU_TENANT_ROLE_READONLY, 10, 5000);
    hu_tenant_create(store, &t);
    hu_tenant_t out = {0};
    hu_tenant_get(store, "ro", &out);
    HU_ASSERT_EQ((int)out.role, (int)HU_TENANT_ROLE_READONLY);
    hu_tenant_store_destroy(store);
}

static void test_tenant_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tenant_store_t *store = NULL;
    hu_tenant_store_init(&alloc, &store);
    hu_tenant_t out = {0};
    HU_ASSERT_NEQ(hu_tenant_get(store, "nonexistent", &out), HU_OK);
    HU_ASSERT_FALSE(hu_tenant_check_quota(store, "nonexistent"));
    HU_ASSERT_FALSE(hu_tenant_check_rate_limit(store, "nonexistent"));
    hu_tenant_store_destroy(store);
}

static void test_tenant_null_args(void) {
    HU_ASSERT_NEQ(hu_tenant_store_init(NULL, NULL), HU_OK);
    HU_ASSERT_NEQ(hu_tenant_create(NULL, NULL), HU_OK);
    HU_ASSERT_NEQ(hu_tenant_get(NULL, NULL, NULL), HU_OK);
}

void run_tenant_tests(void) {
    HU_TEST_SUITE("tenant");
    HU_RUN_TEST(test_tenant_store_init_destroy);
    HU_RUN_TEST(test_tenant_crud);
    HU_RUN_TEST(test_tenant_duplicate_rejected);
    HU_RUN_TEST(test_tenant_list);
    HU_RUN_TEST(test_tenant_quota_enforcement);
    HU_RUN_TEST(test_tenant_rate_limit);
    HU_RUN_TEST(test_tenant_unlimited_quota);
    HU_RUN_TEST(test_tenant_readonly_role);
    HU_RUN_TEST(test_tenant_not_found);
    HU_RUN_TEST(test_tenant_null_args);
}
