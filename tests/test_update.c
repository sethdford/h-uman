#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/update.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <string.h>

/* ── hu_version_compare tests ───────────────────────────────────────── */

static void version_compare_equal(void) {
    HU_ASSERT_EQ(hu_version_compare("1.2.3", "1.2.3"), 0);
}

static void version_compare_equal_with_v_prefix(void) {
    HU_ASSERT_EQ(hu_version_compare("v1.2.3", "1.2.3"), 0);
    HU_ASSERT_EQ(hu_version_compare("1.2.3", "v1.2.3"), 0);
    HU_ASSERT_EQ(hu_version_compare("v1.2.3", "v1.2.3"), 0);
}

static void version_compare_major_diff(void) {
    HU_ASSERT_TRUE(hu_version_compare("2.0.0", "1.0.0") > 0);
    HU_ASSERT_TRUE(hu_version_compare("1.0.0", "2.0.0") < 0);
}

static void version_compare_minor_diff(void) {
    HU_ASSERT_TRUE(hu_version_compare("1.2.0", "1.1.0") > 0);
    HU_ASSERT_TRUE(hu_version_compare("1.1.0", "1.2.0") < 0);
}

static void version_compare_patch_diff(void) {
    HU_ASSERT_TRUE(hu_version_compare("1.0.2", "1.0.1") > 0);
    HU_ASSERT_TRUE(hu_version_compare("1.0.1", "1.0.2") < 0);
}

static void version_compare_numeric_not_lexicographic(void) {
    HU_ASSERT_TRUE(hu_version_compare("0.10.0", "0.9.0") > 0);
    HU_ASSERT_TRUE(hu_version_compare("1.0.0", "0.99.99") > 0);
}

static void version_compare_null_handling(void) {
    HU_ASSERT_EQ(hu_version_compare(NULL, NULL), 0);
    HU_ASSERT_TRUE(hu_version_compare(NULL, "1.0.0") < 0);
    HU_ASSERT_TRUE(hu_version_compare("1.0.0", NULL) > 0);
}

static void version_compare_zero_versions(void) {
    HU_ASSERT_EQ(hu_version_compare("0.0.0", "0.0.0"), 0);
    HU_ASSERT_TRUE(hu_version_compare("0.0.1", "0.0.0") > 0);
}

/* ── hu_update_check (mock path) ────────────────────────────────────── */

static void update_check_returns_mock_version(void) {
    char buf[64];
    hu_error_t err = hu_update_check(buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(buf, "99.99.99");
}

static void update_check_null_buffer_returns_error(void) {
    hu_error_t err = hu_update_check(NULL, 64);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void update_check_zero_size_returns_error(void) {
    char buf[4];
    hu_error_t err = hu_update_check(buf, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ── hu_update_maybe_check ──────────────────────────────────────────── */

static void maybe_check_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    HU_ASSERT_EQ(hu_update_maybe_check(NULL, &cfg), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_update_maybe_check(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void maybe_check_off_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char au[] = "off";
    cfg.auto_update = au;
    HU_ASSERT_EQ(hu_update_maybe_check(&alloc, &cfg), HU_OK);
}

static void maybe_check_null_auto_update_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_update = NULL;
    HU_ASSERT_EQ(hu_update_maybe_check(&alloc, &cfg), HU_OK);
}

/* ── config field parsing ───────────────────────────────────────────── */

static void config_parse_auto_update_field(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_allocator_t alloc = hu_system_allocator();
    cfg.allocator = alloc;

    const char *json = "{\"auto_update\": \"check\", \"update_check_interval_hours\": 12}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.auto_update);
    HU_ASSERT_STR_EQ(cfg.auto_update, "check");
    HU_ASSERT_EQ(cfg.update_check_interval_hours, 12u);

    if (cfg.auto_update)
        alloc.free(alloc.ctx, cfg.auto_update, strlen(cfg.auto_update) + 1);
}

static void config_parse_auto_update_apply(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_allocator_t alloc = hu_system_allocator();
    cfg.allocator = alloc;

    const char *json = "{\"auto_update\": \"apply\"}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.auto_update);
    HU_ASSERT_STR_EQ(cfg.auto_update, "apply");

    if (cfg.auto_update)
        alloc.free(alloc.ctx, cfg.auto_update, strlen(cfg.auto_update) + 1);
}

static void config_defaults_auto_update_off(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    char home[512];
    HU_ASSERT_TRUE(hu_test_tmppath(home, sizeof(home), "human_update_test_noconfig"));
    setenv("HOME", home, 1);
    hu_error_t err = hu_config_load(&alloc, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.auto_update);
    HU_ASSERT_STR_EQ(cfg.auto_update, "off");
    HU_ASSERT_EQ(cfg.update_check_interval_hours, 24u);
    hu_config_deinit(&cfg);
}

/* ── suite registration ─────────────────────────────────────────────── */

void run_update_tests(void) {
    HU_TEST_SUITE("update");
    HU_RUN_TEST(version_compare_equal);
    HU_RUN_TEST(version_compare_equal_with_v_prefix);
    HU_RUN_TEST(version_compare_major_diff);
    HU_RUN_TEST(version_compare_minor_diff);
    HU_RUN_TEST(version_compare_patch_diff);
    HU_RUN_TEST(version_compare_numeric_not_lexicographic);
    HU_RUN_TEST(version_compare_null_handling);
    HU_RUN_TEST(version_compare_zero_versions);
    HU_RUN_TEST(update_check_returns_mock_version);
    HU_RUN_TEST(update_check_null_buffer_returns_error);
    HU_RUN_TEST(update_check_zero_size_returns_error);
    HU_RUN_TEST(maybe_check_null_args_returns_error);
    HU_RUN_TEST(maybe_check_off_returns_ok);
    HU_RUN_TEST(maybe_check_null_auto_update_returns_ok);
    HU_RUN_TEST(config_parse_auto_update_field);
    HU_RUN_TEST(config_parse_auto_update_apply);
    HU_RUN_TEST(config_defaults_auto_update_off);
}
