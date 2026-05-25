/* Phase 4b (Gemma throughput program) — doctor checks for the
 * inference env-var configuration. Pins the operator-visible behavior
 * for each failure mode the factory silently absorbs.
 *
 * Contract under test (per the public docstring on
 * hu_doctor_check_inference in include/human/doctor.h):
 *
 *   - HU_LLAMACPP_KV_QUANT unset                    → OK line
 *   - HU_LLAMACPP_KV_QUANT=q8_0 (recognized)        → OK line
 *   - HU_LLAMACPP_KV_QUANT=q3_0 (typo)              → WARN line
 *   - HU_LLAMACPP_FLASH_ATTN unset                  → OK line
 *   - HU_LLAMACPP_FLASH_ATTN=off                    → OK line
 *   - HU_LLAMACPP_FLASH_ATTN=OFF (case mismatch)    → WARN line
 *   - HU_LLAMACPP_DRAFT_MODEL=/no/such/file         → ERR line
 *   - HU_LLAMACPP_DRAFT_MIN_P=2.0 (out of range)    → WARN line
 *   - HU_LLAMACPP_DRAFT_MAX_TOKENS=999 (out range)  → WARN line
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

/* Helper: find the first item whose message contains `needle`. */
static const hu_diag_item_t *find_item(const hu_diag_item_t *items, size_t count,
                                       const char *needle) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].message && strstr(items[i].message, needle))
            return &items[i];
    }
    return NULL;
}

/* Match the canonical caller pattern from src/doctor.c (line ~971):
 * doctor_push_line doubles cap as needed, but only when cap > 0
 * (otherwise 0 * 2 = 0 → zero-byte alloc → heap overflow). Tests must
 * pre-allocate. 8 slots is enough for any single hu_doctor_check_*
 * function in this suite. */
#define HU_DOCTOR_TEST_INITIAL_CAP 8

static void init_buf(hu_allocator_t *a, hu_diag_item_t **items, size_t *count, size_t *cap) {
    *cap = HU_DOCTOR_TEST_INITIAL_CAP;
    *count = 0;
    *items = (hu_diag_item_t *)a->alloc(a->ctx, sizeof(hu_diag_item_t) * (*cap));
    HU_ASSERT_NOT_NULL(*items);
}

static void free_items(hu_allocator_t *a, hu_diag_item_t *items, size_t count, size_t cap) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].message)
            a->free(a->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
        if (items[i].category)
            a->free(a->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
    }
    if (items)
        a->free(a->ctx, items, sizeof(hu_diag_item_t) * cap);
}

/* Clear every Phase 1+ inference env var so each test starts fresh. */
static void clear_inference_env(void) {
    unsetenv("HU_LLAMACPP_KV_QUANT");
    unsetenv("HU_LLAMACPP_FLASH_ATTN");
    unsetenv("HU_LLAMACPP_DRAFT_MODEL");
    unsetenv("HU_LLAMACPP_DRAFT_MIN_P");
    unsetenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");
}

static void test_doctor_inference_all_unset_reports_defaults_OK(void) {
    clear_inference_env();
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(count >= 3); /* one per kv_quant / flash_attn / draft_model at minimum */

    const hu_diag_item_t *kv = find_item(items, count, "HU_LLAMACPP_KV_QUANT: unset");
    HU_ASSERT_NOT_NULL(kv);
    HU_ASSERT_EQ((int)kv->severity, (int)HU_DIAG_OK);

    const hu_diag_item_t *fa = find_item(items, count, "HU_LLAMACPP_FLASH_ATTN: unset");
    HU_ASSERT_NOT_NULL(fa);
    HU_ASSERT_EQ((int)fa->severity, (int)HU_DIAG_OK);

    const hu_diag_item_t *dm = find_item(items, count, "HU_LLAMACPP_DRAFT_MODEL: unset");
    HU_ASSERT_NOT_NULL(dm);
    HU_ASSERT_EQ((int)dm->severity, (int)HU_DIAG_OK);

    free_items(&a, items, count, cap);
}

static void test_doctor_inference_kv_quant_typo_emits_warn(void) {
    /* The HIGHEST-LEVERAGE check in the whole module: a typo silently
     * falls back to FP16 at the factory, wasting a bench-day. Doctor
     * must surface it. */
    clear_inference_env();
    setenv("HU_LLAMACPP_KV_QUANT", "q3_0", 1); /* not a real variant */
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *warn = find_item(items, count, "UNRECOGNIZED");
    HU_ASSERT_NOT_NULL(warn);
    HU_ASSERT_EQ((int)warn->severity, (int)HU_DIAG_WARN);
    /* Message must include the actual value the operator set, so they
     * know which env var to fix without grepping their shell history. */
    HU_ASSERT_TRUE(strstr(warn->message, "q3_0") != NULL);
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_KV_QUANT");
}

static void test_doctor_inference_kv_quant_canonical_is_OK(void) {
    clear_inference_env();
    setenv("HU_LLAMACPP_KV_QUANT", "q8_0", 1);
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *ok = find_item(items, count, "recognized");
    HU_ASSERT_NOT_NULL(ok);
    HU_ASSERT_EQ((int)ok->severity, (int)HU_DIAG_OK);
    /* No UNRECOGNIZED line should appear. */
    HU_ASSERT_TRUE(find_item(items, count, "UNRECOGNIZED") == NULL);
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_KV_QUANT");
}

static void test_doctor_inference_flash_attn_case_mismatch_warns(void) {
    /* "OFF" / "No" / "disable" — operator clearly meant to disable but
     * the factory's off-token list is strictly lowercase. */
    clear_inference_env();
    setenv("HU_LLAMACPP_FLASH_ATTN", "OFF", 1);
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *warn = find_item(items, count, "factory keeps FA ON");
    HU_ASSERT_NOT_NULL(warn);
    HU_ASSERT_EQ((int)warn->severity, (int)HU_DIAG_WARN);
    HU_ASSERT_TRUE(strstr(warn->message, "OFF") != NULL); /* echoes the operator's value */
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_FLASH_ATTN");
}

static void test_doctor_inference_draft_model_missing_file_errs(void) {
    /* The most severe case: spec decode silently disabled at provider
     * creation. ERR (not WARN) because the operator explicitly opted
     * in and got nothing. */
    clear_inference_env();
    setenv("HU_LLAMACPP_DRAFT_MODEL", "/tmp/this-path-does-not-exist-phase-4b.gguf", 1);
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *err = find_item(items, count, "NOT READABLE");
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_EQ((int)err->severity, (int)HU_DIAG_ERR);
    /* Hint message must point at the fetch script. */
    HU_ASSERT_TRUE(strstr(err->message, "scripts/fetch-gemma.sh --draft") != NULL);
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_DRAFT_MODEL");
}

static void test_doctor_inference_draft_min_p_out_of_range_warns(void) {
    clear_inference_env();
    setenv("HU_LLAMACPP_DRAFT_MIN_P", "2.0", 1); /* > 1.0 */
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *warn = find_item(items, count, "DRAFT_MIN_P");
    HU_ASSERT_NOT_NULL(warn);
    HU_ASSERT_EQ((int)warn->severity, (int)HU_DIAG_WARN);
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_DRAFT_MIN_P");
}

static void test_doctor_inference_draft_max_tokens_out_of_range_warns(void) {
    clear_inference_env();
    setenv("HU_LLAMACPP_DRAFT_MAX_TOKENS", "9999", 1); /* >= 64 */
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    init_buf(&a, &items, &count, &cap);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, &cap), HU_OK);
    const hu_diag_item_t *warn = find_item(items, count, "DRAFT_MAX_TOKENS");
    HU_ASSERT_NOT_NULL(warn);
    HU_ASSERT_EQ((int)warn->severity, (int)HU_DIAG_WARN);
    free_items(&a, items, count, cap);
    unsetenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");
}

static void test_doctor_inference_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_diag_item_t *items = NULL;
    size_t count = 0, cap = 0;
    HU_ASSERT_EQ(hu_doctor_check_inference(NULL, &items, &count, &cap), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, NULL, &count, &cap), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, NULL, &cap), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_check_inference(&a, &items, &count, NULL), HU_ERR_INVALID_ARGUMENT);
}

void run_doctor_inference_tests(void) {
    HU_TEST_SUITE("doctor_inference");
    HU_RUN_TEST(test_doctor_inference_all_unset_reports_defaults_OK);
    HU_RUN_TEST(test_doctor_inference_kv_quant_typo_emits_warn);
    HU_RUN_TEST(test_doctor_inference_kv_quant_canonical_is_OK);
    HU_RUN_TEST(test_doctor_inference_flash_attn_case_mismatch_warns);
    HU_RUN_TEST(test_doctor_inference_draft_model_missing_file_errs);
    HU_RUN_TEST(test_doctor_inference_draft_min_p_out_of_range_warns);
    HU_RUN_TEST(test_doctor_inference_draft_max_tokens_out_of_range_warns);
    HU_RUN_TEST(test_doctor_inference_rejects_null_args);
}
