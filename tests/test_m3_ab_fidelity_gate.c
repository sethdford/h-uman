/* tests/test_m3_ab_fidelity_gate.c
 *
 * Spec 2026-05-19 M3 closure / AC-M3-5 — coverage for the new
 * fidelity-based A/B gate at src/ml/m3_ab_fidelity_gate.c.
 *
 * Contracts pinned (positive-shape per
 * .claude/rules/tests-that-pin-bugs.md):
 *
 *   1. Pure predicate boundary: hu_m3_ab_fidelity_pass returns true
 *      iff (candidate - baseline) >= threshold AND threshold >= 0
 *      AND all inputs are finite.
 *   2. NaN / inf inputs to the predicate → false.
 *   3. JSONL scorer reads "response" field and skips rows that lack it.
 *   4. End-to-end gate: candidate clearly more "Seth-like" than baseline
 *      → PASS with positive delta.
 *   5. End-to-end gate: candidate is the same as baseline → no-change
 *      (delta < threshold) → FAIL.
 *   6. End-to-end gate: zero scored on either side → FAIL with named
 *      reason.
 *
 * Gate symmetry (per .claude/rules/test-source-gate-symmetry.md):
 *   The source is in HU_CORE_SOURCES gated by HU_ENABLE_ML (sibling of
 *   fidelity.c). We use the internal-#ifdef-wrap-with-stub-runner
 *   pattern so this test source stays in the unconditional
 *   HU_TEST_SOURCES list without breaking the no-ML variant builds.
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/core/allocator.h"
#include "human/memory/personal_model.h"
#include "human/ml/m3_ab_fidelity_gate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* Build a "casual Seth"-shaped fingerprint as the scoring target.
 * Matches the synthetic fallback in src/ml/fidelity.c so the units
 * stay calibrated. */
static hu_communication_style_t casual_target(void) {
    hu_communication_style_t t;
    memset(&t, 0, sizeof(t));
    t.formality = 0.3f;
    t.verbosity = 0.5f;
    t.emoji_frequency = 0.2f;
    t.humor_receptivity = 0.6f;
    t.lowercase_ratio = 0.85f;
    t.abbreviation_ratio = 0.2f;
    t.avg_message_length = 60;
    t.sample_count = 1U;
    return t;
}

static int write_responses(const char *path, const char *const *responses, size_t n) {
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    for (size_t i = 0; i < n; i++) {
        fprintf(fp, "{\"response\": \"%s\"}\n", responses[i]);
    }
    fclose(fp);
    return 0;
}

/* ── 1. Pure predicate threshold boundary ─────────────────────────────── */

static void fidelity_pass_predicate_threshold_boundary(void) {
    /* Equal delta and threshold → pass (>=). */
    HU_ASSERT_TRUE(hu_m3_ab_fidelity_pass(0.5f, 0.55f, 0.05f));
    /* Just under threshold → fail. */
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(0.5f, 0.54f, 0.05f));
    /* Negative delta → fail. */
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(0.7f, 0.5f, 0.05f));
    /* Zero threshold: any positive delta passes. */
    HU_ASSERT_TRUE(hu_m3_ab_fidelity_pass(0.5f, 0.5001f, 0.0f));
    /* Negative threshold → invalid → fail. */
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(0.5f, 0.9f, -0.1f));
}

/* ── 2. NaN / inf reject ─────────────────────────────────────────────── */

static void fidelity_pass_predicate_rejects_nonfinite(void) {
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(NAN, 0.5f, 0.05f));
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(0.5f, NAN, 0.05f));
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(0.5f, 0.5f, NAN));
    HU_ASSERT_FALSE(hu_m3_ab_fidelity_pass(INFINITY, 0.5f, 0.05f));
}

/* ── 3. JSONL scorer skips rows missing "response" ───────────────────── */

static void scorer_skips_rows_missing_response_field(void) {
    hu_allocator_t alloc = A();
    const char *path = "/tmp/hu_m3_ab_scorer_missing.jsonl";
    (void)unlink(path);
    FILE *fp = fopen(path, "w");
    HU_ASSERT_NOT_NULL(fp);
    fprintf(fp, "{\"response\": \"yeah ok\"}\n");
    fprintf(fp, "{\"prompt\": \"only prompt, no response\"}\n");
    fprintf(fp, "{\"response\": \"\"}\n");
    fprintf(fp, "not json at all\n");
    fclose(fp);

    hu_communication_style_t target = casual_target();
    hu_communication_style_set_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    hu_error_t err = hu_m3_ab_score_responses_jsonl(&alloc, &target, path, &summary);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(summary.scored, 1U);
    HU_ASSERT_TRUE(summary.skipped >= 2U);

    (void)unlink(path);
}

/* ── 4. End-to-end PASS when candidate beats baseline by threshold ────── */

static void gate_pass_when_candidate_clearly_better(void) {
    hu_allocator_t alloc = A();
    const char *baseline_path = "/tmp/hu_m3_ab_baseline.jsonl";
    const char *candidate_path = "/tmp/hu_m3_ab_candidate.jsonl";
    (void)unlink(baseline_path);
    (void)unlink(candidate_path);

    /* Baseline: very long, ALL UPPERCASE, no abbreviations — maximally
     * far from the casual target (lowercase_ratio=0.85, abbrev=0.2,
     * avg_len=60). */
    const char *baseline[] = {
        "I WILL BE AVAILABLE LATER THIS EVENING TO DISCUSS THE PROJECT IN "
        "GREAT DETAIL WITH YOU AND ANYONE ELSE WHO WISHES TO ATTEND.",
        "THANK YOU FOR YOUR MESSAGE TODAY AND I HOPE YOU ARE HAVING A "
        "WONDERFUL DAY FILLED WITH PRODUCTIVITY AND POSITIVE EXPERIENCES.",
        "I SHALL PROVIDE A COMPREHENSIVE RESPONSE TO YOUR INQUIRY AT MY "
        "EARLIEST CONVENIENCE WHICH WILL LIKELY BE WITHIN A FEW HOURS.",
    };
    /* Candidate: ~60-char lowercase responses with abbreviations —
     * close fit to the casual target. */
    const char *candidate[] = {
        "ya around later prob after dinner if that works for u lol",
        "thx for the heads up, ya kinda thinking the same tbh haha",
        "ya ill get back to u soon, just gotta finish this one thing rn",
    };
    HU_ASSERT_EQ(write_responses(baseline_path, baseline, 3), 0);
    HU_ASSERT_EQ(write_responses(candidate_path, candidate, 3), 0);

    hu_communication_style_t target = casual_target();
    hu_m3_ab_fidelity_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err =
        hu_m3_ab_run_fidelity_gate(&alloc, &target, baseline_path, candidate_path, 0.05f, &report);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(report.candidate.mean > report.baseline.mean);
    /* The casual responses should clear the 0.05 threshold. */
    HU_ASSERT_TRUE(report.pass);
    HU_ASSERT_TRUE(report.candidate.scored == 3U);
    HU_ASSERT_TRUE(report.baseline.scored == 3U);

    (void)unlink(baseline_path);
    (void)unlink(candidate_path);
}

/* ── 5. FAIL when candidate is the same as baseline ──────────────────── */

static void gate_fail_when_candidate_equals_baseline(void) {
    hu_allocator_t alloc = A();
    const char *baseline_path = "/tmp/hu_m3_ab_dup_b.jsonl";
    const char *candidate_path = "/tmp/hu_m3_ab_dup_c.jsonl";
    (void)unlink(baseline_path);
    (void)unlink(candidate_path);

    const char *same[] = {
        "ya around",
        "thx",
        "ok",
    };
    HU_ASSERT_EQ(write_responses(baseline_path, same, 3), 0);
    HU_ASSERT_EQ(write_responses(candidate_path, same, 3), 0);

    hu_communication_style_t target = casual_target();
    hu_m3_ab_fidelity_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err =
        hu_m3_ab_run_fidelity_gate(&alloc, &target, baseline_path, candidate_path, 0.05f, &report);
    HU_ASSERT_EQ(err, HU_OK);
    /* Delta is zero (same responses) → below 0.05 threshold → FAIL. */
    HU_ASSERT_FALSE(report.pass);
    HU_ASSERT_NOT_NULL((void *)report.reason);

    (void)unlink(baseline_path);
    (void)unlink(candidate_path);
}

/* ── 6. FAIL when one side has zero scored ───────────────────────────── */

static void gate_fail_when_empty_jsonl(void) {
    hu_allocator_t alloc = A();
    const char *empty = "/tmp/hu_m3_ab_empty.jsonl";
    const char *full = "/tmp/hu_m3_ab_full.jsonl";
    (void)unlink(empty);
    (void)unlink(full);
    FILE *fp = fopen(empty, "w");
    HU_ASSERT_NOT_NULL(fp);
    fclose(fp);
    const char *good[] = {"ya", "ok"};
    HU_ASSERT_EQ(write_responses(full, good, 2), 0);

    hu_communication_style_t target = casual_target();
    hu_m3_ab_fidelity_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err = hu_m3_ab_run_fidelity_gate(&alloc, &target, empty, full, 0.05f, &report);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(report.pass);
    HU_ASSERT_NOT_NULL((void *)report.reason);
    HU_ASSERT_EQ(report.baseline.scored, 0U);

    (void)unlink(empty);
    (void)unlink(full);
}

void run_m3_ab_fidelity_gate_tests(void);
void run_m3_ab_fidelity_gate_tests(void) {
    HU_TEST_SUITE("m3_ab_fidelity_gate");
    HU_RUN_TEST(fidelity_pass_predicate_threshold_boundary);
    HU_RUN_TEST(fidelity_pass_predicate_rejects_nonfinite);
    HU_RUN_TEST(scorer_skips_rows_missing_response_field);
    HU_RUN_TEST(gate_pass_when_candidate_clearly_better);
    HU_RUN_TEST(gate_fail_when_candidate_equals_baseline);
    HU_RUN_TEST(gate_fail_when_empty_jsonl);
}

#else /* !HU_ENABLE_ML — stub runner so the symbol resolves */

void run_m3_ab_fidelity_gate_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_ML */
