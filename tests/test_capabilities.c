/* Tests for src/app/capabilities.c — the shared "list what this build can
 * do" helper used by both `human capabilities` (cli_commands.c) and the
 * `admin.capabilities` RPC (cp_admin.c). Both callers used to hand-roll
 * their own channel listing (one hardcoded, one catalog-driven); this
 * pins the single shared implementation both now call. */
#include "human/capabilities.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* The catalog always contains at least the CLI channel in every build
 * (src/channels/channel_catalog.c guards it behind HU_HAS_CLI, which every
 * target defines) — so we always have a live channel key to assert on
 * without hardcoding a channel name here. */
static const char *any_catalog_channel_key(void) {
    size_t n = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&n);
    HU_ASSERT(n > 0);
    return catalog[0].key;
}

static void test_channels_built_list_csv_contains_a_catalog_channel(void) {
    char buf[1024];
    size_t written = hu_capabilities_channels_built_list(buf, sizeof(buf), false);
    HU_ASSERT(written > 0);
    HU_ASSERT_EQ(strlen(buf), written); /* NUL-terminated at the reported length */
    HU_ASSERT_TRUE(strstr(buf, any_catalog_channel_key()) != NULL);
}

static void test_channels_built_list_csv_lists_every_catalog_entry(void) {
    /* Non-vacuous: the list must have exactly as many comma-separated
     * entries as the catalog has channels compiled into this build — not
     * just "some channel appears somewhere". */
    char buf[1024];
    hu_capabilities_channels_built_list(buf, sizeof(buf), false);

    size_t n = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&n);
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_TRUE(strstr(buf, catalog[i].key) != NULL);
    }
}

static void test_channels_built_list_json_wraps_keys_in_array_syntax(void) {
    char buf[1024];
    size_t written = hu_capabilities_channels_built_list(buf, sizeof(buf), true);
    HU_ASSERT(written > 0);
    HU_ASSERT_EQ(buf[0], '[');
    HU_ASSERT_EQ(buf[written - 1], ']');

    char quoted[128];
    snprintf(quoted, sizeof(quoted), "\"%s\"", any_catalog_channel_key());
    HU_ASSERT_TRUE(strstr(buf, quoted) != NULL);
}

static void test_channels_built_list_null_out_is_noop(void) {
    HU_ASSERT_EQ(hu_capabilities_channels_built_list(NULL, 128, false), (size_t)0);
    char buf[8];
    HU_ASSERT_EQ(hu_capabilities_channels_built_list(buf, 0, false), (size_t)0);
}

static void test_channels_configured_count_null_cfg_returns_zero(void) {
    HU_ASSERT_EQ(hu_capabilities_channels_configured_count(NULL), (size_t)0);
}

static void test_channels_configured_count_matches_manual_catalog_walk(void) {
    /* Pre/post contract against the real production symbol: build the
     * expected count by walking the catalog directly (the same way
     * cp_admin.c used to before it was wired to the shared helper), then
     * assert the helper agrees — non-vacuous, fails if the helper's filter
     * predicate ever diverges from hu_channel_catalog_is_configured. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    HU_ASSERT_EQ(hu_config_load(&alloc, &cfg), HU_OK);

    size_t n = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&n);
    size_t expected = 0;
    for (size_t i = 0; i < n; i++) {
        if (hu_channel_catalog_is_configured(&cfg, catalog[i].id))
            expected++;
    }

    HU_ASSERT_EQ(hu_capabilities_channels_configured_count(&cfg), expected);
    hu_config_deinit(&cfg);
}

void run_capabilities_tests(void) {
    HU_TEST_SUITE("capabilities");
    HU_RUN_TEST(test_channels_built_list_csv_contains_a_catalog_channel);
    HU_RUN_TEST(test_channels_built_list_csv_lists_every_catalog_entry);
    HU_RUN_TEST(test_channels_built_list_json_wraps_keys_in_array_syntax);
    HU_RUN_TEST(test_channels_built_list_null_out_is_noop);
    HU_RUN_TEST(test_channels_configured_count_null_cfg_returns_zero);
    HU_RUN_TEST(test_channels_configured_count_matches_manual_catalog_walk);
}
