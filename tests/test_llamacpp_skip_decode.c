/*
 * Phase 2b.2 (Gemma throughput program) — skip-decode predicate truth table.
 *
 * src/providers/llamacpp.c:llamacpp_chat_with_system takes the cache-hit
 * skip-decode shortcut iff ALL four conditions hold:
 *   1. sys_hit
 *   2. operator_opt_in (config->kvcache_skip_decode)
 *   3. cached_n_past > 0
 *   4. n_tokens > cached_n_past
 *
 * The decision lives at the chat path inside the HU_LLAMACPP_LINKED gate
 * and is unreachable from tests via the chat surface — the test preset
 * stubs libllama, so the real chat function returns NOT_SUPPORTED before
 * the predicate ever fires.
 *
 * Per .claude/rules/security-predicate-extraction.md, the decision was
 * extracted into `hu_llamacpp_should_skip_decode` — a pure function
 * exported from the same TU. The chat path calls it; this suite pins
 * every branch of the 4-input truth table.
 *
 * Why this matters: mis-firing the skip path corrupts the KV cache and
 * produces wrong output. Mis-NOT-firing is just a missed perf win.
 * The risk asymmetry means the predicate's contract must be regression-
 * locked — flipping any branch silently would ship as latent KV
 * corruption the moment HU_LLAMACPP_LINKED ships.
 */

#include "human/providers/llamacpp.h"

#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>

/* ── All-true: the ONLY combination that returns true ─────────────── */

static void test_skip_decode_all_four_conditions_returns_true(void) {
    HU_ASSERT_TRUE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                  /*cached=*/10, /*n_tokens=*/20));
}

/* ── Single-input falsification: each input flipped → false ───────── */

static void test_skip_decode_blocked_when_no_sys_hit(void) {
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/false, /*opt_in=*/true,
                                                   /*cached=*/10, /*n_tokens=*/20));
}

static void test_skip_decode_blocked_when_operator_not_opted_in(void) {
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/false,
                                                   /*cached=*/10, /*n_tokens=*/20));
}

static void test_skip_decode_blocked_when_cached_n_past_is_zero(void) {
    /* Boundary: a hit with zero cached tokens means nothing to skip. */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                   /*cached=*/0, /*n_tokens=*/20));
}

static void test_skip_decode_blocked_when_n_tokens_equals_cached(void) {
    /* Boundary: no user portion to decode after the cached prefix.
     * Per the comment in llamacpp.c, the sampler needs at least one
     * token to sample from — skipping here would deadlock. */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                   /*cached=*/15, /*n_tokens=*/15));
}

static void test_skip_decode_blocked_when_n_tokens_less_than_cached(void) {
    /* Pathological: new prompt is shorter than the cached prefix.
     * Shouldn't normally happen but the predicate must not negative-
     * size the user-portion submission to llama_decode. */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                   /*cached=*/20, /*n_tokens=*/10));
}

/* ── Multi-input falsification: covers the bulk of the 16-row table ─ */

static void test_skip_decode_blocked_when_nothing_configured(void) {
    /* Operator hasn't opted in, no cache hit — defensive default. */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/false, /*opt_in=*/false,
                                                   /*cached=*/0, /*n_tokens=*/20));
}

static void test_skip_decode_blocked_on_negative_cached_n_past(void) {
    /* int32_t guards against negative values surviving from a stale
     * cache state. cached_n_past > 0 is strict, not non-zero. */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                   /*cached=*/-1, /*n_tokens=*/20));
}

static void test_skip_decode_blocked_when_hit_but_opt_out(void) {
    /* The Phase 2b safe-default case: cache hit fires, savings counter
     * accumulates upstream, but the operator hasn't opted in. Predicate
     * must return false so the SAFE path runs (clear + full re-decode). */
    HU_ASSERT_FALSE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/false,
                                                   /*cached=*/5, /*n_tokens=*/12));
}

/* ── Positive edge: minimum-viable skip ───────────────────────────── */

static void test_skip_decode_fires_at_minimum_viable_user_portion(void) {
    /* cached=1, n_tokens=2 → user portion is exactly 1 token, the
     * smallest case that still has work for llama_decode to do. */
    HU_ASSERT_TRUE(hu_llamacpp_should_skip_decode(/*sys_hit=*/true, /*opt_in=*/true,
                                                  /*cached=*/1, /*n_tokens=*/2));
}

void run_llamacpp_skip_decode_tests(void) {
    HU_TEST_SUITE("llamacpp_skip_decode");
    HU_RUN_TEST(test_skip_decode_all_four_conditions_returns_true);
    HU_RUN_TEST(test_skip_decode_blocked_when_no_sys_hit);
    HU_RUN_TEST(test_skip_decode_blocked_when_operator_not_opted_in);
    HU_RUN_TEST(test_skip_decode_blocked_when_cached_n_past_is_zero);
    HU_RUN_TEST(test_skip_decode_blocked_when_n_tokens_equals_cached);
    HU_RUN_TEST(test_skip_decode_blocked_when_n_tokens_less_than_cached);
    HU_RUN_TEST(test_skip_decode_blocked_when_nothing_configured);
    HU_RUN_TEST(test_skip_decode_blocked_on_negative_cached_n_past);
    HU_RUN_TEST(test_skip_decode_blocked_when_hit_but_opt_out);
    HU_RUN_TEST(test_skip_decode_fires_at_minimum_viable_user_portion);
}
