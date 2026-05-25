#include "human/core/allocator.h"
#include "human/vertex_adc.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* In HU_IS_TEST builds, hu_vertex_adc_token returns a deterministic fake
 * token without touching disk or network. Tests exercise the public contract
 * — NULL handling, output ownership, default project resolution. */

static void test_token_null_alloc_returns_invalid_argument(void) {
    char *tok = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_vertex_adc_token(NULL, &tok, &tlen), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(tok == NULL);
}

static void test_token_null_out_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_vertex_adc_token(&alloc, NULL, &tlen), HU_ERR_INVALID_ARGUMENT);
}

static void test_token_returns_nonempty_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *tok = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_vertex_adc_token(&alloc, &tok, &tlen), HU_OK);
    HU_ASSERT_NOT_NULL(tok);
    HU_ASSERT(tlen > 0);
    /* Token must be NUL-terminated so callers can pass it to snprintf. */
    HU_ASSERT_EQ(tok[tlen], '\0');
    alloc.free(alloc.ctx, tok, tlen + 1);
}

static void test_default_project_env_overrides_adc(void) {
    /* GOOGLE_CLOUD_PROJECT env wins — matches src/tools/media_*.c convention. */
    setenv("GOOGLE_CLOUD_PROJECT", "test-env-project", 1);
    hu_allocator_t alloc = hu_system_allocator();
    const char *p = hu_vertex_adc_default_project(&alloc);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "test-env-project");
    unsetenv("GOOGLE_CLOUD_PROJECT");
}

static void test_default_project_returns_stub_when_no_env(void) {
    unsetenv("GOOGLE_CLOUD_PROJECT");
    hu_allocator_t alloc = hu_system_allocator();
    const char *p = hu_vertex_adc_default_project(&alloc);
    /* Test stub returns "test-project" when no env is set. */
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "test-project");
}

static void test_reset_cache_is_safe_to_call(void) {
    /* Just exercises the test-only reset path — no observable behavior in
     * HU_IS_TEST builds because tokens aren't cached, but the call must not
     * segfault. */
    hu_vertex_adc_reset_cache_for_test();
    hu_vertex_adc_reset_cache_for_test();
}

void run_vertex_adc_tests(void);
void run_vertex_adc_tests(void) {
    HU_TEST_SUITE("vertex_adc");
    HU_RUN_TEST(test_token_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(test_token_null_out_returns_invalid_argument);
    HU_RUN_TEST(test_token_returns_nonempty_token);
    HU_RUN_TEST(test_default_project_env_overrides_adc);
    HU_RUN_TEST(test_default_project_returns_stub_when_no_env);
    HU_RUN_TEST(test_reset_cache_is_safe_to_call);
}
