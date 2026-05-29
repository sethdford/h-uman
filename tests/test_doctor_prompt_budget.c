/* tests/test_doctor_prompt_budget.c
 *
 * Unit tests for the Sprint 55 B3 prompt-budget doctor check
 * (src/doctor/check_prompt_budget.c). The check reports the
 * operator-facing state of the prompt-budget pipeline.
 *
 * These tests exercise the deterministic, no-config code paths of the
 * check's vtable runner directly — no daemon, no snapshot file, no
 * network. They pin the documented ctx contract:
 *
 *   - NULL ctx                       -> NA, "no config provided"
 *   - ctx with NULL cfg              -> NA, "no config provided"
 *   - ctx with cfg, enabled=false    -> NA, reason names "enabled=false"
 *
 * The enabled=true paths read ~/.human/prompt_budget.snapshot.json and
 * depend on filesystem/daemon state, so they are intentionally NOT
 * exercised here (they are non-deterministic without a fixture daemon).
 */

// @covers-none — exercises the real hu_doctor_check_prompt_budget vtable global from
// src/doctor/check_prompt_budget.c; the test-reference basename heuristic resolves
// "test_doctor_prompt_budget" to src/doctor.c (wrong file) and can't reach the check_*.c
// source, so this opt-out is required despite the test calling production code directly.

#include "test_framework.h"

#include "human/config.h"
#include "human/doctor/check.h"
#include "human/doctor/check_prompt_budget.h"

#include <string.h>

/* NULL ctx must yield NA with the documented "no config" reason and
 * never dereference a null pointer. */
static void test_prompt_budget_null_ctx_returns_na(void) {
    hu_doctor_check_result_t result =
        hu_doctor_check_prompt_budget.run(&hu_doctor_check_prompt_budget, NULL);

    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(result.reason);
    HU_ASSERT(strstr(result.reason, "no config provided") != NULL);
}

/* A ctx whose cfg pointer is NULL takes the same NA path as a NULL ctx. */
static void test_prompt_budget_null_cfg_returns_na(void) {
    hu_doctor_check_prompt_budget_ctx_t pctx = {.cfg = NULL};

    hu_doctor_check_result_t result =
        hu_doctor_check_prompt_budget.run(&hu_doctor_check_prompt_budget, &pctx);

    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(result.reason);
    HU_ASSERT(strstr(result.reason, "no config provided") != NULL);
}

/* A config with prompt_budget.enabled=false yields NA and the reason
 * must name the disabled gate so an operator knows how to turn it on. */
static void test_prompt_budget_disabled_returns_na_naming_gate(void) {
    /* A zero-initialized config has prompt_budget.enabled = false. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.prompt_budget.enabled = false;

    hu_doctor_check_prompt_budget_ctx_t pctx = {.cfg = &cfg};

    hu_doctor_check_result_t result =
        hu_doctor_check_prompt_budget.run(&hu_doctor_check_prompt_budget, &pctx);

    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(result.reason);
    HU_ASSERT(strstr(result.reason, "enabled=false") != NULL);
    /* detail_json is a borrowed static buffer; must be populated. */
    HU_ASSERT_NOT_NULL(result.detail_json);
    HU_ASSERT(strstr(result.detail_json, "\"snapshot_present\":false") != NULL);
}

void run_doctor_prompt_budget_tests(void) {
    HU_TEST_SUITE("doctor_prompt_budget");
    HU_RUN_TEST(test_prompt_budget_null_ctx_returns_na);
    HU_RUN_TEST(test_prompt_budget_null_cfg_returns_na);
    HU_RUN_TEST(test_prompt_budget_disabled_returns_na_naming_gate);
}
