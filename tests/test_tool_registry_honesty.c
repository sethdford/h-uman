/* test_tool_registry_honesty.c — verifies that the default tool registry
 * does NOT advertise tools that would always return canned failures.
 *
 * Contract (src/tools/factory.c):
 *   1. "twitter" only registers when twitter API credentials are configured
 *      (hu_config_t::twitter has bearer_token, api_key, or access_token set).
 *   2. When credentials are missing, the factory increments
 *      hu_tools_factory_twitter_skipped_count() and emits a one-shot warning.
 *   3. "lsp" never registers in the default factory because src/tools/lsp.c is
 *      a canned stub; hu_tools_factory_lsp_skipped_count() increments on every
 *      hu_tools_create_default call.
 *
 * Counters are reset before each test via
 * hu_tools_factory_reset_honesty_counters so suite ordering does not affect
 * one-shot-warning observation.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/tool.h"
#include "human/tools/factory.h"
#include "test_framework.h"
#include <string.h>

/* Find a tool by name in the registry. Returns NULL if absent. */
static const hu_tool_t *find_tool_by_name(const hu_tool_t *tools, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (!tools[i].vtable || !tools[i].vtable->name)
            continue;
        const char *tname = tools[i].vtable->name(tools[i].ctx);
        if (tname && strcmp(tname, name) == 0)
            return &tools[i];
    }
    return NULL;
}

static void test_twitter_absent_when_no_credentials_configured(void) {
    hu_tools_factory_reset_honesty_counters();
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    /* Default factory call: NULL config means no credentials are reachable. */
    hu_error_t err = hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tools);
    HU_ASSERT(count > 0);
    HU_ASSERT(find_tool_by_name(tools, count, "twitter") == NULL);
    HU_ASSERT_EQ(hu_tools_factory_twitter_skipped_count(), 1u);
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_twitter_present_when_credentials_configured(void) {
    hu_tools_factory_reset_honesty_counters();
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t config;
    memset(&config, 0, sizeof(config));
    /* Bearer token is enough on its own — twitter.c can call the REST API
     * with just the bearer for read endpoints and post-on-behalf with the
     * full quartet. Either way, credentials are "present" for registration. */
    char bearer_buf[] = "AAAAAAAAAAAAAAAAAAAAAAtest_bearer_for_unit_test";
    config.channels.twitter.bearer_token = bearer_buf;
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_tools_create_default(&alloc, ".", 1, NULL, &config, NULL, NULL, NULL, NULL,
                                             NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tools);
    HU_ASSERT(count > 0);
    HU_ASSERT(find_tool_by_name(tools, count, "twitter") != NULL);
    HU_ASSERT_EQ(hu_tools_factory_twitter_skipped_count(), 0u);
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_lsp_never_registered_in_default_factory(void) {
    hu_tools_factory_reset_honesty_counters();
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tools);
    HU_ASSERT(find_tool_by_name(tools, count, "lsp") == NULL);
    HU_ASSERT_EQ(hu_tools_factory_lsp_skipped_count(), 1u);
    hu_tools_destroy_default(&alloc, tools, count);
}

/* Sanity check: the one-shot warning guard means the second call within the
 * same process must NOT re-emit the warning, but it MUST still increment the
 * skipped counter (counter and warning are independent signals). */
static void test_twitter_skip_counter_increments_across_calls(void) {
    hu_tools_factory_reset_honesty_counters();
    hu_allocator_t alloc = hu_system_allocator();
    for (unsigned i = 0; i < 3; i++) {
        hu_tool_t *tools = NULL;
        size_t count = 0;
        hu_error_t err = hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL,
                                                 NULL, NULL, &tools, &count);
        HU_ASSERT_EQ(err, HU_OK);
        hu_tools_destroy_default(&alloc, tools, count);
    }
    HU_ASSERT_EQ(hu_tools_factory_twitter_skipped_count(), 3u);
    HU_ASSERT_EQ(hu_tools_factory_lsp_skipped_count(), 3u);
}

void run_tool_registry_honesty_tests(void) {
    HU_TEST_SUITE("Tool Registry Honesty");
    HU_RUN_TEST(test_twitter_absent_when_no_credentials_configured);
    HU_RUN_TEST(test_twitter_present_when_credentials_configured);
    HU_RUN_TEST(test_lsp_never_registered_in_default_factory);
    HU_RUN_TEST(test_twitter_skip_counter_increments_across_calls);
}
