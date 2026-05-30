/* tests/test_eval_score.c
 *
 * Unit tests for src/eval/eval_score.c — the `human eval score` pure core
 * hu_eval_score_jsonl(). Proves the JSONL path actually drives the C scorers
 * (AC-2) and that axes appear/omit correctly. Exercises the in-memory core
 * directly — no file I/O, no argv.
 *
 * 2026-05-29 — humanness north-star metric, Phase 2.
 */

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/eval/eval_score.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <string.h>

/* Helper: parse the emitted JSON and pull axes.<axis>.<field>. */
static double axis_field(hu_allocator_t *alloc, const char *json, const char *axis,
                         const char *field, double dflt) {
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, json, strlen(json), &root) != HU_OK || !root)
        return dflt;
    double v = dflt;
    hu_json_value_t *axes = hu_json_object_get(root, "axes");
    if (axes) {
        hu_json_value_t *ax = hu_json_object_get(axes, axis);
        if (ax)
            v = hu_json_get_number(ax, field, dflt);
    }
    hu_json_free(alloc, root);
    return v;
}

static double top_number(hu_allocator_t *alloc, const char *json, const char *key, double dflt) {
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, json, strlen(json), &root) != HU_OK || !root)
        return dflt;
    double v = hu_json_get_number(root, key, dflt);
    hu_json_free(alloc, root);
    return v;
}

static bool axis_available(hu_allocator_t *alloc, const char *json, const char *axis) {
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, json, strlen(json), &root) != HU_OK || !root)
        return false;
    bool avail = false;
    hu_json_value_t *axes = hu_json_object_get(root, "axes");
    if (axes) {
        hu_json_value_t *ax = hu_json_object_get(axes, axis);
        if (ax)
            avail = hu_json_get_bool(ax, "available", false);
    }
    hu_json_free(alloc, root);
    return avail;
}

static void test_score_counts_rows_and_anti_ai(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *jsonl = "{\"reply\":\"yeah sounds good\",\"channel\":\"imessage\"}\n"
                        "{\"reply\":\"ok cool\",\"channel\":\"imessage\"}\n";
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, jsonl, strlen(jsonl), NULL, &out, NULL), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(top_number(&a, out, "n", -1) == 2);
    HU_ASSERT_TRUE(axis_field(&a, out, "anti_ai", "n", -1) == 2);
    double m = axis_field(&a, out, "anti_ai", "mean", -1);
    HU_ASSERT_TRUE(m >= 0.0 && m <= 1.0);
    a.free(a.ctx, out, 0);
}

static void test_score_blank_and_malformed_lines_skipped(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *jsonl = "{\"reply\":\"hey\"}\n"
                        "\n"
                        "this is not json\n"
                        "   \n"
                        "{\"reply\":\"yo\"}\n";
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, jsonl, strlen(jsonl), NULL, &out, NULL), HU_OK);
    /* Only the two valid {reply} rows count. */
    HU_ASSERT_TRUE(top_number(&a, out, "n", -1) == 2);
    a.free(a.ctx, out, 0);
}

static void test_score_relationship_axis_only_when_target_present(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *jsonl = "{\"reply\":\"hey love!\",\"channel\":\"imessage\","
                        "\"target_register\":{\"formality\":0.1,\"warmth\":0.9}}\n"
                        "{\"reply\":\"ok\",\"channel\":\"imessage\"}\n"; /* no target_register */
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, jsonl, strlen(jsonl), NULL, &out, NULL), HU_OK);
    HU_ASSERT_TRUE(top_number(&a, out, "n", -1) == 2);
    /* Only the first row contributes to the relationship axis. */
    HU_ASSERT_TRUE(axis_field(&a, out, "relationship", "n", -1) == 1);
    a.free(a.ctx, out, 0);
}

static void test_score_relationship_calibrated_beats_miscalibrated(void) {
    hu_allocator_t a = hu_system_allocator();
    /* Calibrated: casual+warm reply to a warm contact. */
    const char *good = "{\"reply\":\"hey love! can't wait to see you xo\","
                       "\"target_register\":{\"formality\":0.1,\"warmth\":0.9}}\n";
    /* Miscalibrated: stiff reply to the same warm contact. */
    const char *bad = "{\"reply\":\"Received. I will revert regarding this matter in due course.\","
                      "\"target_register\":{\"formality\":0.1,\"warmth\":0.9}}\n";
    char *og = NULL, *ob = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, good, strlen(good), NULL, &og, NULL), HU_OK);
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, bad, strlen(bad), NULL, &ob, NULL), HU_OK);
    double g = axis_field(&a, og, "relationship", "mean", -1);
    double b = axis_field(&a, ob, "relationship", "mean", -1);
    HU_ASSERT_TRUE(g > b); /* the C scorer really ran end-to-end */
    HU_ASSERT_TRUE(g > 0.6);
    HU_ASSERT_TRUE(b < 0.5);
    a.free(a.ctx, og, 0);
    a.free(a.ctx, ob, 0);
}

static void test_score_fidelity_unavailable_without_target_style(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *jsonl = "{\"reply\":\"hey\"}\n";
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, jsonl, strlen(jsonl), NULL, &out, NULL), HU_OK);
    HU_ASSERT_EQ((int)axis_available(&a, out, "fidelity"), 0);
    a.free(a.ctx, out, 0);
}

static void test_score_fidelity_available_with_target_style(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_communication_style_t style;
    memset(&style, 0, sizeof(style));
    style.formality = 0.2f;
    style.verbosity = 0.3f;
    style.lowercase_ratio = 0.8f;
    style.abbreviation_ratio = 0.4f;
    style.avg_message_length = 40;
    style.sample_count = 50; /* >0 so the scorer has a fingerprint */

    const char *jsonl = "{\"reply\":\"yeah just sent it\"}\n"
                        "{\"reply\":\"ha typical\"}\n";
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, jsonl, strlen(jsonl), &style, &out, NULL), HU_OK);
    HU_ASSERT_EQ((int)axis_available(&a, out, "fidelity"), 1);
    HU_ASSERT_TRUE(axis_field(&a, out, "fidelity", "n", -1) >= 1);
    double fm = axis_field(&a, out, "fidelity", "mean", -1);
    HU_ASSERT_TRUE(fm >= 0.0 && fm <= 1.0);
    a.free(a.ctx, out, 0);
}

static void test_score_null_args_rejected(void) {
    hu_allocator_t a = hu_system_allocator();
    char *out = NULL;
    HU_ASSERT_EQ(hu_eval_score_jsonl(&a, NULL, 0, NULL, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_score_jsonl(NULL, "x", 1, NULL, &out, NULL), HU_ERR_INVALID_ARGUMENT);
}

void run_eval_score_tests(void) {
    HU_TEST_SUITE("eval score (humanness composite core)");
    HU_RUN_TEST(test_score_counts_rows_and_anti_ai);
    HU_RUN_TEST(test_score_blank_and_malformed_lines_skipped);
    HU_RUN_TEST(test_score_relationship_axis_only_when_target_present);
    HU_RUN_TEST(test_score_relationship_calibrated_beats_miscalibrated);
    HU_RUN_TEST(test_score_fidelity_unavailable_without_target_style);
    HU_RUN_TEST(test_score_fidelity_available_with_target_style);
    HU_RUN_TEST(test_score_null_args_rejected);
}
