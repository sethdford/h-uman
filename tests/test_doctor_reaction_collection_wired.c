/* tests/test_doctor_reaction_collection_wired.c
 *
 * Chip A from the 2026-05-26 audit follow-up — verdict matrix for the
 * doctor check that catches the silent-failure case where
 * cfg->reaction_collection.enabled=true but the binary was built with
 * HU_ENABLE_RL_FULL=OFF (DPO recorder compiled out).
 *
 * Three verdicts, three tests, plus one for the NULL-cfg path. Uses
 * the explicit-override test seam so we can cover BOTH the production
 * shape and the silent-fail shape from the same binary (the production
 * runner reads HU_ENABLE_RL_FULL from a compile-time #ifdef which the
 * test binary cannot change per-call). */

#include "test_framework.h"

#include "human/config.h"
#include "human/doctor/check.h"
#include "human/doctor/check_reaction_collection_wired.h"

#include <string.h>

static void test_null_cfg_returns_na(void) {
    /* No config at all → NA. Operator gets a clear "skipped" reason
     * rather than a confusing FAIL when running doctor against a path
     * that didn't load a config file. */
    hu_doctor_check_result_t r = hu_doctor_check_reaction_collection_wired_run_for_test(NULL, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_TRUE(strstr(r.reason, "no config") != NULL);
}

static void test_cfg_enabled_false_returns_na(void) {
    /* Operator opted out of reaction_collection — nothing to check.
     * NA is the right verdict (not FAIL); we don't want to nag about
     * a feature the operator deliberately turned off. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = false;
    hu_doctor_check_result_t r = hu_doctor_check_reaction_collection_wired_run_for_test(&cfg, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_TRUE(strstr(r.reason, "enabled=false") != NULL);
    /* detail_json should still appear with the two facts. */
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"reaction_collection_enabled\":false") != NULL);
}

static void test_cfg_enabled_but_not_built_with_rl_full_returns_fail(void) {
    /* THE silent-failure case this whole check exists to catch — the
     * exact 2026-05-26 audit scenario before F#4 landed. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    hu_doctor_check_result_t r =
        hu_doctor_check_reaction_collection_wired_run_for_test(&cfg, /*built_with_rl_full=*/false);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT_NOT_NULL(r.reason);
    /* Reason MUST name the exact rebuild command — operator should NOT
     * have to grep CMakeLists.txt to figure out the fix. */
    HU_ASSERT_TRUE(strstr(r.reason, "HU_ENABLE_RL_FULL") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "cmake -B build -DHU_ENABLE_RL_FULL=ON") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "restart the daemon") != NULL);
    /* detail_json carries both facts so dashboards / alerts can pattern
     * on them without parsing the human-readable reason. */
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"reaction_collection_enabled\":true") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"built_with_rl_full\":false") != NULL);
}

static void test_cfg_enabled_and_built_with_rl_full_returns_pass(void) {
    /* The healthy-production shape. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    hu_doctor_check_result_t r =
        hu_doctor_check_reaction_collection_wired_run_for_test(&cfg, /*built_with_rl_full=*/true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_TRUE(strstr(r.reason, "wired") != NULL ||
                   strstr(r.reason, "AND binary built with HU_ENABLE_RL_FULL") != NULL);
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"built_with_rl_full\":true") != NULL);
}

static void test_vtable_metadata_is_stable(void) {
    /* Name + description are part of the wire contract — JSON consumers
     * key on .name and the registry's check_name accessor returns it.
     * If someone renames the check, every downstream alert breaks. */
    HU_ASSERT_NOT_NULL(hu_doctor_check_reaction_collection_wired.name);
    HU_ASSERT_STR_EQ(hu_doctor_check_reaction_collection_wired.name, "reaction_collection_wired");
    HU_ASSERT_NOT_NULL(hu_doctor_check_reaction_collection_wired.description);
    HU_ASSERT_NOT_NULL(hu_doctor_check_reaction_collection_wired.run);
    /* No autofix yet — operator must rebuild OR edit config; either is
     * out of scope for a `--fix` automation. */
    HU_ASSERT_NULL(hu_doctor_check_reaction_collection_wired.fix);
}

void run_doctor_reaction_collection_wired_tests(void);
void run_doctor_reaction_collection_wired_tests(void) {
    HU_TEST_SUITE("doctor_reaction_collection_wired");
    HU_RUN_TEST(test_null_cfg_returns_na);
    HU_RUN_TEST(test_cfg_enabled_false_returns_na);
    HU_RUN_TEST(test_cfg_enabled_but_not_built_with_rl_full_returns_fail);
    HU_RUN_TEST(test_cfg_enabled_and_built_with_rl_full_returns_pass);
    HU_RUN_TEST(test_vtable_metadata_is_stable);
}
