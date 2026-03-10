/*
 * Vault tests — HU_IS_TEST uses in-memory storage.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security/vault.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void test_vault_set_and_get(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_error_t err = hu_vault_set(v, "my_secret", "secret_value_123");
    HU_ASSERT_EQ(err, HU_OK);

    char out[256];
    err = hu_vault_get(v, "my_secret", out, sizeof(out));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "secret_value_123");

    hu_vault_destroy(v);
}

static void test_vault_get_nonexistent_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    char out[256];
    hu_error_t err = hu_vault_get(v, "nonexistent_key", out, sizeof(out));
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_vault_destroy(v);
}

static void test_vault_delete_removes_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_vault_set(v, "to_delete", "value");
    hu_error_t err = hu_vault_delete(v, "to_delete");
    HU_ASSERT_EQ(err, HU_OK);

    char out[256];
    err = hu_vault_get(v, "to_delete", out, sizeof(out));
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_vault_destroy(v);
}

static void test_vault_delete_nonexistent_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_error_t err = hu_vault_delete(v, "nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_vault_destroy(v);
}

static void test_vault_list_keys_returns_correct_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_vault_set(v, "a", "1");
    hu_vault_set(v, "b", "2");
    hu_vault_set(v, "c", "3");

    char *keys[16];
    size_t count = 0;
    hu_error_t err = hu_vault_list_keys(v, keys, 16, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);

    for (size_t i = 0; i < count; i++)
        alloc.free(alloc.ctx, keys[i], strlen(keys[i]) + 1);

    hu_vault_destroy(v);
}

static void test_vault_no_key_still_works(void) {
    /* With HUMAN_VAULT_KEY unset, vault uses base64 obfuscation. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_error_t err = hu_vault_set(v, "obfuscated", "hello");
    HU_ASSERT_EQ(err, HU_OK);

    char out[256];
    err = hu_vault_get(v, "obfuscated", out, sizeof(out));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "hello");

    hu_vault_destroy(v);
}

static void test_vault_get_api_key_from_vault(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    hu_vault_set(v, "openai_api_key", "sk-vault-key");
    char *api_key = NULL;
    hu_error_t err = hu_vault_get_api_key(v, &alloc, "openai", NULL, &api_key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(api_key);
    HU_ASSERT_STR_EQ(api_key, "sk-vault-key");
    alloc.free(alloc.ctx, api_key, strlen(api_key) + 1);

    hu_vault_destroy(v);
}

static void test_vault_get_api_key_fallback_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    char *api_key = NULL;
    hu_error_t err = hu_vault_get_api_key(v, &alloc, "openai", "sk-config-key", &api_key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(api_key, "sk-config-key");
    alloc.free(alloc.ctx, api_key, strlen(api_key) + 1);

    hu_vault_destroy(v);
}

static void test_vault_get_api_key_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vault_t *v = hu_vault_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(v);

    char *api_key = NULL;
    hu_error_t err = hu_vault_get_api_key(v, &alloc, "openai", NULL, &api_key);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(api_key);

    hu_vault_destroy(v);
}

void run_vault_tests(void) {
    HU_TEST_SUITE("vault");
    HU_RUN_TEST(test_vault_set_and_get);
    HU_RUN_TEST(test_vault_get_nonexistent_returns_not_found);
    HU_RUN_TEST(test_vault_delete_removes_key);
    HU_RUN_TEST(test_vault_delete_nonexistent_returns_not_found);
    HU_RUN_TEST(test_vault_list_keys_returns_correct_count);
    HU_RUN_TEST(test_vault_no_key_still_works);
    HU_RUN_TEST(test_vault_get_api_key_from_vault);
    HU_RUN_TEST(test_vault_get_api_key_fallback_config);
    HU_RUN_TEST(test_vault_get_api_key_not_found);
}
