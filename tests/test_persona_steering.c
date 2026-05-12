/*
 * tests/test_persona_steering.c — SOTA-2026 init-01 S1.
 *
 * Pins the determinism, NULL-handling, threshold-gating, retry-boost, and
 * axis-ordering contract of `hu_persona_steering_vector` and
 * `hu_persona_steering_directive`. No I/O, no clock — every test passes its
 * own `now` and a tracking allocator so leaks fail loudly.
 */

#include "human/persona/steering.h"

#include "human/core/allocator.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "test_framework.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void steering_zero_vec(float *v) {
    for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++)
        v[i] = 0.0f;
}

/* Build a "warm casual humorous direct" persona with stable string content.
 * Keep all fields stack/static — `hu_persona_steering_vector` only reads;
 * no `hu_persona_deinit` needed. */
static void steering_build_warm_persona(hu_persona_t *p) {
    memset(p, 0, sizeof(*p));
    /* emotional_range.ceiling = "warm and tender" → warmth ≈ (0.8 + 0.6) / 2
     * but we use floor too: ceiling+floor average; here only ceiling so /2
     * yields 0.7 → clamped to <= 1.0. */
    p->emotional_range.ceiling = (char *)"warm and tender";
    p->humor.frequency = (char *)"frequent";        /* 0.4 */
    p->conflict_style.confrontation_comfort = (char *)"direct"; /* +0.6 → hedging = -0.6 */
    /* Provide a casual overlay so formality lands negative. */
    static hu_persona_overlay_t overlay;
    memset(&overlay, 0, sizeof(overlay));
    overlay.formality = (char *)"casual";  /* -0.5 */
    p->overlays = &overlay;
    p->overlays_count = 1;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_persona_steering_vector_rejects_wrong_dim(void) {
    float vec[HU_STEERING_VEC_DIM];
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, NULL, 0, vec, 16),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, NULL, 0, vec, 64),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, NULL, 0, NULL, HU_STEERING_VEC_DIM),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_persona_steering_vector_zero_persona_yields_zero(void) {
    float vec[HU_STEERING_VEC_DIM];
    /* Pre-fill with non-zero junk so we know the function zeroes the buffer. */
    for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++)
        vec[i] = 1.234f;
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, NULL, 0, vec, HU_STEERING_VEC_DIM),
                 HU_OK);
    for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++)
        HU_ASSERT_FLOAT_EQ(vec[i], 0.0f, 1e-6);
}

static void test_persona_steering_vector_axis_order_is_stable(void) {
    hu_persona_t p;
    steering_build_warm_persona(&p);
    float a[HU_STEERING_VEC_DIM];
    float b[HU_STEERING_VEC_DIM];
    steering_zero_vec(a);
    steering_zero_vec(b);
    HU_ASSERT_EQ(hu_persona_steering_vector(&p, NULL, 0, a, HU_STEERING_VEC_DIM), HU_OK);
    HU_ASSERT_EQ(hu_persona_steering_vector(&p, NULL, 0, b, HU_STEERING_VEC_DIM), HU_OK);
    /* Bit-identical second run. */
    for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++)
        HU_ASSERT_FLOAT_EQ(a[i], b[i], 0.0);
    /* Warmth and humor positive; hedging negative; formality negative. */
    HU_ASSERT_TRUE(a[HU_STEERING_AXIS_WARMTH] > 0.2f);
    HU_ASSERT_TRUE(a[HU_STEERING_AXIS_HUMOR_DENSITY] > 0.2f);
    HU_ASSERT_TRUE(a[HU_STEERING_AXIS_HEDGING] < -0.2f);
    HU_ASSERT_TRUE(a[HU_STEERING_AXIS_FORMALITY] < -0.2f);
    /* Style-derived slots are zero because no personal_model passed. */
    HU_ASSERT_FLOAT_EQ(a[HU_STEERING_AXIS_VERBOSITY], 0.0f, 1e-6);
    HU_ASSERT_FLOAT_EQ(a[HU_STEERING_AXIS_EMOJI_AFFINITY], 0.0f, 1e-6);
}

static void test_persona_steering_vector_clamps_to_unit_interval(void) {
    /* Persona that hits the strongest positive coefficient on every persona-
     * derived axis AND the strongest 0..1 input on every style axis. The
     * implementation must never let an axis exceed [-1, +1]. */
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.emotional_range.ceiling = (char *)"tender";
    p.emotional_range.floor   = (char *)"warm";
    p.core_anchor             = (char *)"warm";        /* triple-warmth */
    p.humor.frequency         = (char *)"constant";    /* 0.7 */
    p.conflict_style.confrontation_comfort = (char *)"confrontational"; /* +0.65 */

    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.style.formality        = 1.0f;
    m.style.verbosity        = 1.0f;
    m.style.emoji_frequency  = 1.0f;
    m.style.humor_receptivity = 1.0f;
    m.style.lowercase_ratio  = 1.0f;
    m.style.abbreviation_ratio = 1.0f;
    m.style.avg_message_length = 120;
    m.style.sample_count     = 100;
    m.style.last_observed_at = 1700000000LL;

    float vec[HU_STEERING_VEC_DIM];
    steering_zero_vec(vec);
    HU_ASSERT_EQ(hu_persona_steering_vector(&p, &m, 1700000000LL, vec, HU_STEERING_VEC_DIM),
                 HU_OK);
    for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++) {
        HU_ASSERT_TRUE(vec[i] <= 1.0f + 1e-6);
        HU_ASSERT_TRUE(vec[i] >= -1.0f - 1e-6);
    }
}

static void test_persona_steering_vector_freshness_decays_style_axes(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.style.formality        = 1.0f; /* extreme to make decay visible */
    m.style.emoji_frequency  = 1.0f;
    m.style.lowercase_ratio  = 1.0f;
    m.style.abbreviation_ratio = 1.0f;
    m.style.humor_receptivity = 0.5f; /* neutral so warmth/humor axes don't move */
    m.style.avg_message_length = 80;
    m.style.sample_count     = 50;
    m.style.last_observed_at = 1700000000LL;

    float fresh[HU_STEERING_VEC_DIM];
    float stale[HU_STEERING_VEC_DIM];
    steering_zero_vec(fresh);
    steering_zero_vec(stale);

    /* Fresh: caller's now == last_observed_at → freshness 1.0. */
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, &m, 1700000000LL, fresh, HU_STEERING_VEC_DIM),
                 HU_OK);
    /* Stale: caller's now is one half-life past last_observed → 0.5. */
    long long stale_now = 1700000000LL + HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    HU_ASSERT_EQ(hu_persona_steering_vector(NULL, &m, stale_now, stale, HU_STEERING_VEC_DIM),
                 HU_OK);

    for (int a = HU_STEERING_AXIS_VERBOSITY; a < HU_STEERING_AXIS__NAMED_COUNT; a++) {
        if (fresh[a] == 0.0f)
            continue; /* unaffected axis */
        /* Stale magnitude must be ≤ 60% of fresh magnitude (allowing slack
         * for the multiplicative freshness factor of ~0.5). */
        float fa = fresh[a] < 0 ? -fresh[a] : fresh[a];
        float sa = stale[a] < 0 ? -stale[a] : stale[a];
        HU_ASSERT_TRUE(sa <= fa * 0.6f + 1e-6);
    }
}

static void test_persona_steering_directive_threshold_gates_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float vec[HU_STEERING_VEC_DIM];
    steering_zero_vec(vec);
    /* All zero — directive must be empty. */
    char *out = (char *)0xDEADBEEF;
    size_t out_len = 42;
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &out,
                                               &out_len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long)out_len, 0L);

    /* One axis below floor — still empty. */
    vec[HU_STEERING_AXIS_WARMTH] = 0.10f; /* below 0.15 floor */
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &out,
                                               &out_len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long)out_len, 0L);
}

static void test_persona_steering_directive_emits_known_leader(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float vec[HU_STEERING_VEC_DIM];
    steering_zero_vec(vec);
    vec[HU_STEERING_AXIS_WARMTH] = 0.5f;
    vec[HU_STEERING_AXIS_FORMALITY] = -0.4f;

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &out,
                                               &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_CONTAINS(out, "## Persona steering");
    HU_ASSERT_STR_CONTAINS(out, "warmer");
    HU_ASSERT_STR_CONTAINS(out, "casual");

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_steering_directive_orders_axes_by_magnitude(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float vec[HU_STEERING_VEC_DIM];
    steering_zero_vec(vec);
    /* Warmth weakest, hedging strongest → hedging sentence appears first. */
    vec[HU_STEERING_AXIS_WARMTH]  = 0.25f;
    vec[HU_STEERING_AXIS_HEDGING] = -0.8f;
    vec[HU_STEERING_AXIS_FORMALITY] = 0.5f;

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &out,
                                               &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);

    /* Hedging-negative phrase mentions "confidence"; formality-positive mentions
     * "formal"; warmth-positive mentions "warmer". Order must be:
     *   1. confidence  (hedging |0.8|)
     *   2. formal      (formality |0.5|)
     *   3. warmer      (warmth |0.25|) */
    const char *p_conf = strstr(out, "confidence");
    const char *p_form = strstr(out, "formal");
    const char *p_warm = strstr(out, "warmer");
    HU_ASSERT_NOT_NULL(p_conf);
    HU_ASSERT_NOT_NULL(p_form);
    HU_ASSERT_NOT_NULL(p_warm);
    HU_ASSERT_TRUE(p_conf < p_form);
    HU_ASSERT_TRUE(p_form < p_warm);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_steering_directive_retry_boost_strengthens(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float vec[HU_STEERING_VEC_DIM];
    steering_zero_vec(vec);
    /* Modest signal — strong enough at boost=1 to cross the floor, but the
     * intensity adverb should be "slightly". After boost=2 the magnitude
     * clamps near 0.6 and the adverb upgrades to "strongly". */
    vec[HU_STEERING_AXIS_WARMTH] = 0.25f;

    char *base = NULL, *boosted = NULL;
    size_t base_len = 0, boosted_len = 0;

    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &base,
                                               &base_len),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 3.0f, &boosted,
                                               &boosted_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(base);
    HU_ASSERT_NOT_NULL(boosted);
    HU_ASSERT_STR_CONTAINS(base, "slightly");
    HU_ASSERT_STR_CONTAINS(boosted, "strongly");
    HU_ASSERT_STR_NOT_CONTAINS(boosted, "slightly");

    alloc.free(alloc.ctx, base, base_len + 1);
    alloc.free(alloc.ctx, boosted, boosted_len + 1);
}

static void test_persona_steering_directive_determinism_byte_identical(void) {
    /* Determinism gate: same persona + same input → byte-identical directive
     * suffix. This is the load-bearing acceptance test for the prompt-half
     * scope of init-01. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    steering_build_warm_persona(&p);

    float v1[HU_STEERING_VEC_DIM];
    float v2[HU_STEERING_VEC_DIM];
    steering_zero_vec(v1);
    steering_zero_vec(v2);
    HU_ASSERT_EQ(hu_persona_steering_vector(&p, NULL, 0, v1, HU_STEERING_VEC_DIM), HU_OK);
    HU_ASSERT_EQ(hu_persona_steering_vector(&p, NULL, 0, v2, HU_STEERING_VEC_DIM), HU_OK);

    char *s1 = NULL, *s2 = NULL;
    size_t l1 = 0, l2 = 0;
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, v1, HU_STEERING_VEC_DIM, 1.0f, &s1, &l1),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, v2, HU_STEERING_VEC_DIM, 1.0f, &s2, &l2),
                 HU_OK);
    HU_ASSERT_NOT_NULL(s1);
    HU_ASSERT_NOT_NULL(s2);
    HU_ASSERT_EQ((long)l1, (long)l2);
    HU_ASSERT_EQ(memcmp(s1, s2, l1), 0);

    alloc.free(alloc.ctx, s1, l1 + 1);
    alloc.free(alloc.ctx, s2, l2 + 1);
}

static void test_persona_steering_directive_rejects_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float vec[HU_STEERING_VEC_DIM] = {0};
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_persona_steering_directive(NULL, vec, HU_STEERING_VEC_DIM, 1.0f, &out, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, NULL, HU_STEERING_VEC_DIM, 1.0f, &out,
                                               &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, 16, 1.0f, &out, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, NULL,
                                               &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &out, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_persona_steering_axis_label_stable(void) {
    HU_ASSERT_STR_EQ(hu_persona_steering_axis_label(HU_STEERING_AXIS_WARMTH), "warmth");
    HU_ASSERT_STR_EQ(hu_persona_steering_axis_label(HU_STEERING_AXIS_FORMALITY), "formality");
    HU_ASSERT_STR_EQ(hu_persona_steering_axis_label(HU_STEERING_AXIS_HUMOR_DENSITY),
                     "humor_density");
    HU_ASSERT_STR_EQ(hu_persona_steering_axis_label(HU_STEERING_AXIS_HEDGING), "hedging");
    HU_ASSERT_STR_EQ(hu_persona_steering_axis_label((hu_steering_axis_t)999), "unknown");
}

void run_persona_steering_tests(void) {
    HU_TEST_SUITE("persona_steering");
    HU_RUN_TEST(test_persona_steering_vector_rejects_wrong_dim);
    HU_RUN_TEST(test_persona_steering_vector_zero_persona_yields_zero);
    HU_RUN_TEST(test_persona_steering_vector_axis_order_is_stable);
    HU_RUN_TEST(test_persona_steering_vector_clamps_to_unit_interval);
    HU_RUN_TEST(test_persona_steering_vector_freshness_decays_style_axes);
    HU_RUN_TEST(test_persona_steering_directive_threshold_gates_empty);
    HU_RUN_TEST(test_persona_steering_directive_emits_known_leader);
    HU_RUN_TEST(test_persona_steering_directive_orders_axes_by_magnitude);
    HU_RUN_TEST(test_persona_steering_directive_retry_boost_strengthens);
    HU_RUN_TEST(test_persona_steering_directive_determinism_byte_identical);
    HU_RUN_TEST(test_persona_steering_directive_rejects_invalid_args);
    HU_RUN_TEST(test_persona_steering_axis_label_stable);
}
