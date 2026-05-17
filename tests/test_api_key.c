#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/providers/api_key.h"
#include "test_framework.h"

static void test_api_key_valid_non_null_key_returns_true(void) {
    HU_ASSERT_TRUE(hu_api_key_valid("sk-test-key", 11));
}

static void test_api_key_valid_empty_string_returns_false(void) {
    HU_ASSERT_FALSE(hu_api_key_valid("", 0));
}

static void test_api_key_valid_whitespace_only_returns_false(void) {
    HU_ASSERT_FALSE(hu_api_key_valid("   ", 3));
    HU_ASSERT_FALSE(hu_api_key_valid("\t\n ", 3));
}

static void test_api_key_valid_trimmed_content_returns_true(void) {
    HU_ASSERT_TRUE(hu_api_key_valid("  sk-key  ", 10));
}

static void test_api_key_valid_null_key_returns_false(void) {
    HU_ASSERT_FALSE(hu_api_key_valid(NULL, 0));
}

static void test_api_key_mask_null_returns_no_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *m = hu_api_key_mask(&alloc, NULL, 0);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m, "[no key]");
    hu_str_free(&alloc, m);
}

static void test_api_key_mask_empty_returns_no_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *m = hu_api_key_mask(&alloc, "", 0);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m, "[no key]");
    hu_str_free(&alloc, m);
}

static void test_api_key_mask_short_key_returns_stars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *m = hu_api_key_mask(&alloc, "ab", 2);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m, "****");
    hu_str_free(&alloc, m);
}

static void test_api_key_mask_four_char_key_returns_stars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *m = hu_api_key_mask(&alloc, "abcd", 4);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m, "****");
    hu_str_free(&alloc, m);
}

static void test_api_key_mask_long_key_returns_first_four_plus_ellipsis(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *m = hu_api_key_mask(&alloc, "sk-abcdefgh", 11);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m, "sk-a...");
    hu_str_free(&alloc, m);
}

static void test_api_key_resolve_explicit_key_returns_trimmed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "openai", 6, "  sk-test  ", 11);
    HU_ASSERT_NOT_NULL(k);
    HU_ASSERT_STR_EQ(k, "sk-test");
    hu_str_free(&alloc, k);
}

static void test_api_key_resolve_explicit_key_no_trim_needed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "openai", 6, "sk-test", 7);
    HU_ASSERT_NOT_NULL(k);
    HU_ASSERT_STR_EQ(k, "sk-test");
    hu_str_free(&alloc, k);
}

static void test_api_key_resolve_null_key_no_env_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "unknown-provider-xyz", 19, NULL, 0);
    HU_ASSERT_NULL(k);
}

static void test_api_key_resolve_empty_key_no_env_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "openai", 6, "", 0);
    HU_ASSERT_NULL(k);
}

static void test_api_key_resolve_whitespace_only_key_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "openai", 6, "   ", 3);
    HU_ASSERT_NULL(k);
}

static void test_api_key_resolve_zero_len_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *k = hu_api_key_resolve(&alloc, "openai", 6, "sk-test", 0);
    HU_ASSERT_NULL(k);
}

/* ── hu_provider_default_api_key_env_name ──
 * Centralized env-var lookup that replaced the strcmp ladder in
 * config_merge.c. Single source of truth for provider → env mapping. */

static void test_env_name_openai_returns_openai_api_key(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("openai", 6), "OPENAI_API_KEY");
}

static void test_env_name_anthropic_returns_anthropic_api_key(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("anthropic", 9), "ANTHROPIC_API_KEY");
}

static void test_env_name_gemini_returns_gemini_api_key(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("gemini", 6), "GEMINI_API_KEY");
}

/* "google" and "vertex" are aliases that resolve to the same env var.
 * Previously these were two extra strcmp branches in config_merge.c. */
static void test_env_name_google_alias_resolves_to_gemini(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("google", 6), "GEMINI_API_KEY");
}

static void test_env_name_vertex_alias_resolves_to_gemini(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("vertex", 6), "GEMINI_API_KEY");
}

static void test_env_name_ollama_returns_ollama_host(void) {
    HU_ASSERT_STR_EQ(hu_provider_default_api_key_env_name("ollama", 6), "OLLAMA_HOST");
}

static void test_env_name_unknown_returns_null(void) {
    HU_ASSERT_NULL(hu_provider_default_api_key_env_name("totally_unknown", 15));
}

static void test_env_name_null_returns_null(void) {
    HU_ASSERT_NULL(hu_provider_default_api_key_env_name(NULL, 0));
}

static void test_env_name_zero_len_returns_null(void) {
    HU_ASSERT_NULL(hu_provider_default_api_key_env_name("openai", 0));
}

void run_api_key_tests(void) {
    HU_TEST_SUITE("ApiKey");
    HU_RUN_TEST(test_api_key_valid_non_null_key_returns_true);
    HU_RUN_TEST(test_api_key_valid_empty_string_returns_false);
    HU_RUN_TEST(test_api_key_valid_whitespace_only_returns_false);
    HU_RUN_TEST(test_api_key_valid_trimmed_content_returns_true);
    HU_RUN_TEST(test_api_key_valid_null_key_returns_false);
    HU_RUN_TEST(test_api_key_mask_null_returns_no_key);
    HU_RUN_TEST(test_api_key_mask_empty_returns_no_key);
    HU_RUN_TEST(test_api_key_mask_short_key_returns_stars);
    HU_RUN_TEST(test_api_key_mask_four_char_key_returns_stars);
    HU_RUN_TEST(test_api_key_mask_long_key_returns_first_four_plus_ellipsis);
    HU_RUN_TEST(test_api_key_resolve_explicit_key_returns_trimmed);
    HU_RUN_TEST(test_api_key_resolve_explicit_key_no_trim_needed);
    HU_RUN_TEST(test_api_key_resolve_null_key_no_env_returns_null);
    HU_RUN_TEST(test_api_key_resolve_empty_key_no_env_returns_null);
    HU_RUN_TEST(test_api_key_resolve_whitespace_only_key_returns_null);
    HU_RUN_TEST(test_api_key_resolve_zero_len_returns_null);

    HU_RUN_TEST(test_env_name_openai_returns_openai_api_key);
    HU_RUN_TEST(test_env_name_anthropic_returns_anthropic_api_key);
    HU_RUN_TEST(test_env_name_gemini_returns_gemini_api_key);
    HU_RUN_TEST(test_env_name_google_alias_resolves_to_gemini);
    HU_RUN_TEST(test_env_name_vertex_alias_resolves_to_gemini);
    HU_RUN_TEST(test_env_name_ollama_returns_ollama_host);
    HU_RUN_TEST(test_env_name_unknown_returns_null);
    HU_RUN_TEST(test_env_name_null_returns_null);
    HU_RUN_TEST(test_env_name_zero_len_returns_null);
}
