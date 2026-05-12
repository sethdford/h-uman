/* test_frontier_prompt.c — coverage for the RL SOTA Phase 1 frontier
 * prompt bundle assembler.
 *
 * Why this file exists: `src/agent/frontier_prompt.c` was committed to
 * `sprint-2c-followups` without any tests/* file referencing it,
 * tripping the CI gate `scripts/check-untested.sh` ("NO TEST:
 * src/agent/frontier_prompt.c") and turning the base branch red for
 * the four S2 commits leading up to the 2026-05-12 gap audit. This
 * file closes that gap with real coverage of the public surface
 * (`hu_frontier_prompt_build`, `hu_frontier_prompt_free`) without
 * requiring a fully-populated agent — the deep cognition layer's
 * own suites cover the sub-builders end-to-end already.
 *
 * Scope:
 *   1. NULL-input validation — the explicit contract from line 331 of
 *      src/agent/frontier_prompt.c (alloc / agent / out → INVALID_ARG).
 *   2. Free is null-safe under all NULL-pointer permutations.
 *   3. Free clears an arbitrarily partially-populated bundle and zeroes
 *      every owned-pointer slot — the production hu_frontier_prompt_free
 *      must touch every one of the 13 ctx fields, in any combination,
 *      without crashing.
 *   4. Round-trip build → free on a zero-initialized agent (only the
 *      allocator wired) — proves the sub-builders all handle missing
 *      persona / memory / sqlite gracefully, and that no field is
 *      leaked by the free path. Pinned under ASan via the suite runner.
 */

#include "human/agent.h"
#include "human/agent/frontier_prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

/* ── NULL-input contract ──────────────────────────────────────────────── */

static void build_null_alloc_returns_invalid_arg(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_frontier_prompt_bundle_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_frontier_prompt_build(NULL, &agent, "hi", 2, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

static void build_null_agent_returns_invalid_arg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_frontier_prompt_bundle_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_frontier_prompt_build(&alloc, NULL, "hi", 2, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

static void build_null_out_returns_invalid_arg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    HU_ASSERT_EQ(hu_frontier_prompt_build(&alloc, &agent, "hi", 2, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Free null-safety contract ────────────────────────────────────────── */

static void free_null_alloc_and_null_bundle_is_safe(void) {
    /* Both NULL → no-op. Calling free on NULL anything must never
     * crash. This is the contract every public free function in
     * h-uman holds. */
    hu_frontier_prompt_free(NULL, NULL);

    hu_allocator_t alloc = hu_system_allocator();
    hu_frontier_prompt_free(&alloc, NULL);

    hu_frontier_prompt_bundle_t b;
    memset(&b, 0, sizeof(b));
    hu_frontier_prompt_free(NULL, &b);
}

static void free_zeroed_bundle_is_idempotent(void) {
    /* A zero-init bundle has every pointer NULL and every length 0;
     * free must do nothing observable and leave the bundle zeroed. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_frontier_prompt_bundle_t b;
    memset(&b, 0, sizeof(b));
    hu_frontier_prompt_free(&alloc, &b);
    HU_ASSERT_NULL(b.humanness_ctx);
    HU_ASSERT_NULL(b.imperfect_dir);
    HU_ASSERT_NULL(b.residue_dir);
    HU_ASSERT_NULL(b.presence_ctx);
    HU_ASSERT_NULL(b.micro_expr_ctx);
    HU_ASSERT_NULL(b.novelty_ctx);
    HU_ASSERT_NULL(b.attachment_ctx);
    HU_ASSERT_NULL(b.rupture_ctx);
    HU_ASSERT_NULL(b.narrative_self_ctx);
    HU_ASSERT_NULL(b.creative_voice_ctx);
    HU_ASSERT_NULL(b.growth_ctx);
    HU_ASSERT_NULL(b.boundary_ctx);
    HU_ASSERT_NULL(b.rel_episode_ctx);
    HU_ASSERT_EQ(b.humanness_ctx_len, 0u);
    HU_ASSERT_EQ(b.rel_episode_ctx_len, 0u);
}

static void free_partially_populated_bundle_clears_every_owned_slot(void) {
    /* Manually populate three non-adjacent ctx slots (humanness,
     * presence, narrative_self), then free. The contract is that
     * every owned pointer is freed and the bundle is zeroed in full,
     * regardless of which fields were set. ASan catches any missed
     * slot at the runner level. */
    hu_allocator_t alloc = hu_system_allocator();

    hu_frontier_prompt_bundle_t b;
    memset(&b, 0, sizeof(b));

    const char *hum = "humanness ctx";
    const size_t hum_len = strlen(hum);
    b.humanness_ctx = (char *)alloc.alloc(alloc.ctx, hum_len + 1);
    HU_ASSERT_NOT_NULL(b.humanness_ctx);
    memcpy(b.humanness_ctx, hum, hum_len + 1);
    b.humanness_ctx_len = hum_len;

    const char *pres = "presence";
    const size_t pres_len = strlen(pres);
    b.presence_ctx = (char *)alloc.alloc(alloc.ctx, pres_len + 1);
    HU_ASSERT_NOT_NULL(b.presence_ctx);
    memcpy(b.presence_ctx, pres, pres_len + 1);
    b.presence_ctx_len = pres_len;

    const char *narr = "narrative self";
    const size_t narr_len = strlen(narr);
    b.narrative_self_ctx = (char *)alloc.alloc(alloc.ctx, narr_len + 1);
    HU_ASSERT_NOT_NULL(b.narrative_self_ctx);
    memcpy(b.narrative_self_ctx, narr, narr_len + 1);
    b.narrative_self_ctx_len = narr_len;

    hu_frontier_prompt_free(&alloc, &b);

    HU_ASSERT_NULL(b.humanness_ctx);
    HU_ASSERT_NULL(b.presence_ctx);
    HU_ASSERT_NULL(b.narrative_self_ctx);
    HU_ASSERT_EQ(b.humanness_ctx_len, 0u);
    HU_ASSERT_EQ(b.presence_ctx_len, 0u);
    HU_ASSERT_EQ(b.narrative_self_ctx_len, 0u);
}

/* ── Round-trip on zero-init agent ────────────────────────────────────── */

static void build_zero_init_agent_round_trips_without_leak(void) {
    /* Pin: a zero-init agent (only `alloc` wired) is the minimum
     * permitted input. The sub-builders must all gracefully handle
     * missing persona, missing memory, and zero frontiers state.
     * The bundle may be partially or fully empty — what matters is
     * that build returns HU_OK and free is leak-clean under ASan. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;

    hu_frontier_prompt_bundle_t out;
    memset(&out, 0, sizeof(out));

    hu_error_t err =
        hu_frontier_prompt_build(&alloc, &agent, "hello", 5, NULL, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);

    /* Whatever ctx fields the sub-builders chose to populate must
     * round-trip cleanly through free. ASan catches any leaked
     * allocation. */
    hu_frontier_prompt_free(&alloc, &out);
    HU_ASSERT_NULL(out.humanness_ctx);
    HU_ASSERT_NULL(out.rel_episode_ctx);
}

static void build_zero_init_agent_with_empty_message_round_trips(void) {
    /* Edge: empty message. msg_len = 0 should not crash any sub-builder. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;

    hu_frontier_prompt_bundle_t out;
    memset(&out, 0, sizeof(out));

    hu_error_t err = hu_frontier_prompt_build(&alloc, &agent, "", 0, NULL, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    hu_frontier_prompt_free(&alloc, &out);
}

/* ── Registration ─────────────────────────────────────────────────────── */

void run_frontier_prompt_tests(void) {
    HU_TEST_SUITE("Frontier Prompt");

    HU_RUN_TEST(build_null_alloc_returns_invalid_arg);
    HU_RUN_TEST(build_null_agent_returns_invalid_arg);
    HU_RUN_TEST(build_null_out_returns_invalid_arg);
    HU_RUN_TEST(free_null_alloc_and_null_bundle_is_safe);
    HU_RUN_TEST(free_zeroed_bundle_is_idempotent);
    HU_RUN_TEST(free_partially_populated_bundle_clears_every_owned_slot);
    HU_RUN_TEST(build_zero_init_agent_round_trips_without_leak);
    HU_RUN_TEST(build_zero_init_agent_with_empty_message_round_trips);
}
