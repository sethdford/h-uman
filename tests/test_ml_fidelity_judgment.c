/* Tests for US-7.6 — judgment-fidelity (INS-A) NLL seam.
 *
 * The seam ships DORMANT in sprint 7 (decision D3 in
 * sprints/sprint-7/decisions.md): no production NLL backend is wired,
 * the default returns HU_ERR_NOT_SUPPORTED, and these tests exercise
 * the plumbing via a deterministic mock injected through
 * `hu_ml_fidelity_set_nll_compute_fn`. No real model weights are
 * loaded — AC-7.6.3 forbids it. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"
#include "human/ml/fidelity.h"
#include "test_framework.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── deterministic mock NLL fns ──────────────────────────────────────── */

typedef struct {
    size_t call_count;
    /* When `degenerate` is false, NLL = base + delta * i (per row).
     * When true, NLL = high (flat, mimicking an adapter that pushed
     * probability mass off the real continuations). */
    double base;
    double delta;
    bool degenerate;
    double degenerate_nll;
} mock_nll_ctx_t;

static hu_error_t mock_nll_increasing(const char *prompt, size_t prompt_len, const char *cont,
                                      size_t cont_len, void *ctx, double *out_nll) {
    (void)prompt;
    (void)cont;
    /* Sanity — the seam must hand non-empty rows to the NLL fn. */
    if (prompt_len == 0 || cont_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    mock_nll_ctx_t *m = (mock_nll_ctx_t *)ctx;
    if (m->degenerate) {
        *out_nll = m->degenerate_nll;
    } else {
        *out_nll = m->base + m->delta * (double)m->call_count;
    }
    m->call_count++;
    return HU_OK;
}

static hu_error_t mock_nll_always_skip(const char *prompt, size_t prompt_len, const char *cont,
                                       size_t cont_len, void *ctx, double *out_nll) {
    (void)prompt;
    (void)prompt_len;
    (void)cont;
    (void)cont_len;
    (void)ctx;
    (void)out_nll;
    return HU_ERR_NOT_SUPPORTED;
}

/* Path resolution: tests may run from build/ depending on harness.
 * The fixture lives at tests/fixtures/... relative to the repo root.
 * Try both relative paths so the test is robust to CWD. */
static const char *resolve_fixture(const char *rel) {
    static char buf[1024];
    /* Prefer the env override hook if set (lets CI/dev wire absolute
     * paths). */
    const char *env = getenv("HU_JUDGMENT_HOLDOUT_TEST_DIR");
    if (env && env[0]) {
        snprintf(buf, sizeof(buf), "%s/%s", env, rel);
        FILE *f = fopen(buf, "rb");
        if (f) {
            fclose(f);
            return buf;
        }
    }
    /* CWD candidates */
    const char *candidates[] = {"tests/fixtures/", "../tests/fixtures/", "./tests/fixtures/", NULL};
    for (size_t i = 0; candidates[i]; i++) {
        snprintf(buf, sizeof(buf), "%s%s", candidates[i], rel);
        FILE *f = fopen(buf, "rb");
        if (f) {
            fclose(f);
            return buf;
        }
    }
    return NULL;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_holdout_loads_at_least_ten_rows(void) {
    /* AC scaffolding: the shipped fixture must contain ≥ 10 well-formed
     * rows; this is the regression-sentinel floor documented in the
     * design doc. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = resolve_fixture("judgment_fidelity_holdout.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    hu_error_t err = hu_ml_fidelity_load_holdout(&alloc, path, &holdout);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GE(holdout.rows_count, (size_t)10);
    /* All rows must have non-empty prompt + continuation. */
    for (size_t i = 0; i < holdout.rows_count; i++) {
        HU_ASSERT_NOT_NULL(holdout.rows[i].prompt);
        HU_ASSERT_NOT_NULL(holdout.rows[i].continuation);
        HU_ASSERT_GT(holdout.rows[i].prompt_len, (size_t)0);
        HU_ASSERT_GT(holdout.rows[i].continuation_len, (size_t)0);
    }
    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_holdout_skips_malformed_rows(void) {
    /* The malformed sibling fixture has 2 good rows, 1 missing-field,
     * and 1 not-json. Loader must keep 2, drop 2 silently. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = resolve_fixture("judgment_fidelity_holdout_malformed.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    hu_error_t err = hu_ml_fidelity_load_holdout(&alloc, path, &holdout);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(holdout.rows_count, (size_t)2);
    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_production_default_returns_not_supported(void) {
    /* AC-7.6.3 (one half): with no mock registered, the seam must
     * return scored=0/available=false, meaning the production default
     * returned HU_ERR_NOT_SUPPORTED for every row and zero real model
     * weights were touched. */
    hu_allocator_t alloc = hu_system_allocator();
    /* Ensure we're using the default. */
    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);

    const char *path = resolve_fixture("judgment_fidelity_holdout.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_load_holdout(&alloc, path, &holdout), HU_OK);

    hu_ml_judgment_summary_t summary = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &summary), HU_OK);
    HU_ASSERT_EQ(summary.scored, (size_t)0);
    HU_ASSERT_EQ(summary.skipped, holdout.rows_count);
    HU_ASSERT_FALSE(summary.available);
    HU_ASSERT_FLOAT_EQ(summary.mean_nll, 0.0, 1e-12);

    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_judgment_ppl_computed_on_holdout(void) {
    /* AC-7.6.1: register a deterministic mock, load the fixture, score
     * every row. Mean NLL must equal the analytical sum / N. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = resolve_fixture("judgment_fidelity_holdout.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_load_holdout(&alloc, path, &holdout), HU_OK);

    mock_nll_ctx_t ctx = {0};
    ctx.base = 0.5;
    ctx.delta = 0.01;
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_increasing, &ctx);

    hu_ml_judgment_summary_t summary = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &summary), HU_OK);

    HU_ASSERT_EQ(summary.scored, holdout.rows_count);
    HU_ASSERT_EQ(summary.skipped, (size_t)0);
    HU_ASSERT_TRUE(summary.available);
    /* Expected mean = base + delta * (n-1)/2 — arithmetic series. */
    double n = (double)summary.scored;
    double expected_mean = ctx.base + ctx.delta * (n - 1.0) / 2.0;
    HU_ASSERT_FLOAT_EQ(summary.mean_nll, expected_mean, 1e-9);
    HU_ASSERT_FLOAT_EQ(summary.min_nll, ctx.base, 1e-12);
    HU_ASSERT_FLOAT_EQ(summary.max_nll, ctx.base + ctx.delta * (n - 1.0), 1e-9);
    HU_ASSERT_EQ(ctx.call_count, holdout.rows_count);

    /* PPL exposed in CLI = exp(mean_nll). Spot-check the convention so
     * the CLI assertion in the shell test matches. */
    double ppl = exp(summary.mean_nll);
    HU_ASSERT_GT(ppl, 0.0);

    /* Cleanup. */
    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);
    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_judgment_ppl_catches_degenerate_adapter(void) {
    /* AC-7.6.2: a "before" mock that scores tight (low NLL) and an
     * "after" mock that scores degenerate (high NLL) must produce a
     * positive delta. The gate (which fails on delta > 0) would catch
     * the adapter even though it might pass surface-style checks. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = resolve_fixture("judgment_fidelity_holdout.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_load_holdout(&alloc, path, &holdout), HU_OK);

    /* "Baseline" pass: flat NLL = 0.5. */
    mock_nll_ctx_t ctx_before = {0};
    ctx_before.base = 0.5;
    ctx_before.delta = 0.0;
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_increasing, &ctx_before);

    hu_ml_judgment_summary_t before = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &before), HU_OK);
    HU_ASSERT_TRUE(before.available);
    HU_ASSERT_FLOAT_EQ(before.mean_nll, 0.5, 1e-9);

    /* "Degenerate adapter" pass: flat NLL = 1.2 — worse on real
     * continuations even though surface metrics could pass. */
    mock_nll_ctx_t ctx_after = {0};
    ctx_after.degenerate = true;
    ctx_after.degenerate_nll = 1.2;
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_increasing, &ctx_after);

    hu_ml_judgment_summary_t after = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &after), HU_OK);
    HU_ASSERT_TRUE(after.available);
    HU_ASSERT_FLOAT_EQ(after.mean_nll, 1.2, 1e-9);

    /* Lower NLL = better. after - before > 0 means the adapter made
     * judgment fidelity WORSE. This is what the gate fails on.
     * (HU_ASSERT_GT casts to long long, so we use HU_ASSERT_TRUE on
     * the comparison directly for doubles.) */
    double delta = after.mean_nll - before.mean_nll;
    HU_ASSERT_TRUE(delta > 0.0);
    /* PPL ratio sanity: exp(1.2)/exp(0.5) >> 1. */
    HU_ASSERT_TRUE(exp(after.mean_nll) / exp(before.mean_nll) > 1.5);

    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);
    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_nll_seam_never_loads_real_weights(void) {
    /* AC-7.6.3 (other half): when the production default is active
     * (no mock), the score path returns skipped == N without ever
     * succeeding. We assert by registering an explicit mock that
     * INCREMENTS a counter — if the production default is ever the
     * code path, the counter would stay at zero. Conversely, with no
     * mock registered, the test already proves the default path
     * (`default_nll_not_supported`) never opens any file or weight
     * blob — its body has no I/O at all. We pin both halves.
     */
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = resolve_fixture("judgment_fidelity_holdout.jsonl");
    HU_ASSERT_NOT_NULL(path);
    hu_ml_judgment_holdout_t holdout = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_load_holdout(&alloc, path, &holdout), HU_OK);

    /* Half 1: with mock registered, mock must be hit exactly N times. */
    mock_nll_ctx_t ctx = {0};
    ctx.base = 0.7;
    ctx.delta = 0.0;
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_increasing, &ctx);
    hu_ml_judgment_summary_t summary = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &summary), HU_OK);
    HU_ASSERT_EQ(ctx.call_count, holdout.rows_count);
    HU_ASSERT_EQ(summary.scored, holdout.rows_count);

    /* Half 2: reset to default, prove the default is HU_ERR_NOT_SUPPORTED
     * — i.e. no real-weights path. */
    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);
    hu_ml_judgment_summary_t default_summary = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &default_summary), HU_OK);
    HU_ASSERT_EQ(default_summary.scored, (size_t)0);
    HU_ASSERT_FALSE(default_summary.available);

    /* Half 3: registering an explicit always-skip mock returns the same
     * shape as the production default, confirming HU_ERR_NOT_SUPPORTED
     * is the contract for "no inference available". */
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_always_skip, NULL);
    hu_ml_judgment_summary_t skip_summary = {0};
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&holdout, &skip_summary), HU_OK);
    HU_ASSERT_EQ(skip_summary.scored, (size_t)0);
    HU_ASSERT_EQ(skip_summary.skipped, holdout.rows_count);
    HU_ASSERT_FALSE(skip_summary.available);

    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);
    hu_ml_fidelity_free_holdout(&alloc, &holdout);
}

static void test_baseline_signature_unchanged_compile_check(void) {
    /* AC-7.6.4: take the address of hu_ml_fidelity_score_baseline and
     * assign it to a function-pointer typed against the *original*
     * signature. If anyone changes the signature, this assignment
     * fails at compile time. */
    hu_error_t (*const fn)(const hu_persona_t *, const hu_communication_style_t *,
                           hu_communication_style_set_summary_t *) = &hu_ml_fidelity_score_baseline;
    HU_ASSERT_NOT_NULL((void *)fn);
    /* Runtime smoke: invalid args path is unchanged. */
    HU_ASSERT_EQ(fn(NULL, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_default_holdout_path_respects_env(void) {
    /* `hu_ml_fidelity_default_holdout_path` returns env override when
     * set; otherwise the in-tree fixture path. */
    const char *prev = getenv("HU_JUDGMENT_HOLDOUT");
    unsetenv("HU_JUDGMENT_HOLDOUT");
    const char *p = hu_ml_fidelity_default_holdout_path();
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_CONTAINS(p, "judgment_fidelity_holdout.jsonl");

    setenv("HU_JUDGMENT_HOLDOUT", "/tmp/some_override.jsonl", 1);
    const char *q = hu_ml_fidelity_default_holdout_path();
    HU_ASSERT_STR_EQ(q, "/tmp/some_override.jsonl");

    if (prev)
        setenv("HU_JUDGMENT_HOLDOUT", prev, 1);
    else
        unsetenv("HU_JUDGMENT_HOLDOUT");
}

static void test_empty_holdout_is_not_supported(void) {
    /* Edge: zero rows must produce available=false but no error. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_ml_judgment_holdout_t empty = {0};
    hu_ml_judgment_summary_t s = {0};
    /* Register the mock so this isn't conflated with the production
     * default. */
    mock_nll_ctx_t ctx = {0};
    ctx.base = 0.5;
    hu_ml_fidelity_set_nll_compute_fn(mock_nll_increasing, &ctx);
    HU_ASSERT_EQ(hu_ml_fidelity_score_judgment(&empty, &s), HU_OK);
    HU_ASSERT_EQ(s.scored, (size_t)0);
    HU_ASSERT_FALSE(s.available);
    HU_ASSERT_EQ(ctx.call_count, (size_t)0);
    hu_ml_fidelity_set_nll_compute_fn(NULL, NULL);
    (void)alloc;
}

void run_ml_fidelity_judgment_tests(void) {
    HU_TEST_SUITE("fidelity_judgment");
    HU_RUN_TEST(test_holdout_loads_at_least_ten_rows);
    HU_RUN_TEST(test_holdout_skips_malformed_rows);
    HU_RUN_TEST(test_production_default_returns_not_supported);
    HU_RUN_TEST(test_judgment_ppl_computed_on_holdout);
    HU_RUN_TEST(test_judgment_ppl_catches_degenerate_adapter);
    HU_RUN_TEST(test_nll_seam_never_loads_real_weights);
    HU_RUN_TEST(test_baseline_signature_unchanged_compile_check);
    HU_RUN_TEST(test_default_holdout_path_respects_env);
    HU_RUN_TEST(test_empty_holdout_is_not_supported);
}
